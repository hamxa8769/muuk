#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "muuk/ctrader_fix/instance_lock.hpp"
#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/news_feed.hpp"
#include "muuk/mt5_bridge/mt5_gateway.hpp"
#include "muuk/mt5_bridge/mt5_launcher.hpp"

namespace muuk::desktop {

// Owns a muuk::ctrader_fix::LiveRunner on a background thread and exposes
// thread-safe accessors for the render (GUI) thread to poll every frame:
// the latest LiveSnapshot, a rolling log buffer, and any startup/runtime
// error. This is the only place app/desktop touches engine/ctrader_fix —
// DashboardWindow only ever talks to this class.
class LiveController {
public:
    LiveController() = default;
    ~LiveController();

    LiveController(const LiveController&) = delete;
    LiveController& operator=(const LiveController&) = delete;

    // Loads the config at `path` and starts LiveRunner::run() on a
    // background thread. No-op if already running. On a config load
    // failure, records the message (readable via lastError()) and starts
    // nothing.
    void start(const std::string& configPath);

    // Stops the running LiveRunner (if any) and joins its thread. Safe to
    // call even if not running. Also ends auto-reconnect until the next start().
    void stop();

    // Auto-reconnect: after start(), a connection that drops by itself
    // (network, broker restart) is retried after 5, 10, 20, 40, then every
    // 60 s -- the saved basket is picked up again on reconnect. Call tick()
    // every frame (it does the retrying, on the caller's thread).
    void setAutoReconnect(bool on) { autoReconnect_ = on; }
    void tick();

    struct ConnectionHealth {
        int reconnects = 0;                  // reconnect attempts since the app started
        std::optional<int> reconnectInSeconds;  // set while one is scheduled
        std::string lastDrop;                // why the last connection ended by itself
        std::string lastDropAt;
    };
    [[nodiscard]] ConnectionHealth connectionHealth() const;

    // MetaTrader 5 bridge (only for a config with "platform": "mt5"): started
    // with the first connect and kept up across reconnects, so the EA in MT5
    // stays connected. nullopt when no bridge is running.
    [[nodiscard]] std::optional<muuk::mt5_bridge::EaStatus> bridgeStatus() const;
    // What starting MetaTrader 5 did ("" = nothing yet), and whether it's still busy (compiling / launching).
    [[nodiscard]] std::string mt5StartMessage() const;
    [[nodiscard]] bool mt5Starting() const { return mt5Starting_.load(); }
    // Starts MT5 with the bridge now (the Connect button does it by itself).
    void startMt5(const std::string& configPath);

    // Range / position controls -- forwarded straight to LiveRunner (see
    // its comments for exactly what each does). No-ops if not running.
    bool openRange();
    // reopen=false: don't open a new range after the cooldown (exit guard).
    void closeRange(bool reopen = true);
    void closeAllPositions();
    void cancelAllPendingOrders();
    void closePosition(muuk::core::OrderId id);
    void cancelOrder(muuk::core::OrderId id);
    void placeWaitingOrderNow(muuk::core::OrderId id);
    void syncPositions();

    // Exit guard (see LiveRunner::flattenAndWait): closes everything and
    // blocks until the account is flat. false if not connected or not
    // confirmed within `timeout`.
    bool flattenAndWait(std::chrono::milliseconds timeout);

    // Applies `settings` to the running bot (from its next range on).
    // No-op if not running.
    void applySettings(const muuk::ctrader_fix::LiveSettings& settings);

    // Reads/writes only the tunable part of a config file, so settings can
    // be edited and saved whether or not the bot is connected. On failure
    // both return false/nullopt and record the message in lastError().
    [[nodiscard]] std::optional<muuk::ctrader_fix::LiveSettings> loadSettings(const std::string& configPath);
    bool saveSettings(const std::string& configPath, const muuk::ctrader_fix::LiveSettings& settings);

    // ForexFactory calendar (news guard + News tab): downloaded in the
    // background, at most hourly, cached in `cacheFile`. Shared with the bot.
    void startNews(const std::filesystem::path& cacheFile);
    [[nodiscard]] std::shared_ptr<muuk::ctrader_fix::NewsFeed> news() const { return news_; }

    // Preview mode (--demo): no broker connection at all; snapshot() returns
    // made-up, moving data so the screens can be looked at and checked.
    void enableDemo() {
        demo_ = true;
        demoStart_ = std::chrono::steady_clock::now();
    }
    [[nodiscard]] bool demo() const { return demo_; }

    [[nodiscard]] bool isRunning() const { return running_.load() || demo_; }
    // Returns the last known state even after the runner has stopped
    // (e.g. a dropped connection), so the UI can show what happened
    // rather than blanking out. std::nullopt only before the first
    // successful start().
    [[nodiscard]] std::optional<muuk::ctrader_fix::LiveSnapshot> snapshot() const;
    [[nodiscard]] std::vector<std::string> logLines() const;
    void clearLog();
    [[nodiscard]] std::string lastError() const;
    void clearError();

    // --- History, reports, logs, saved state ---------------------------------
    // Everything is stored next to the config file, so it's all still there
    // the next time the app opens:
    //   muuk_state/<config>.state.json  the running basket + lifetime statistics
    //   muuk_history/<config>.baskets.jsonl  every closed basket of that account (appended as it closes)
    //   muuk_logs/muuk_live.log         the log, written line by line
    //   muuk_logs/muuk_<time>.log       saved copies (Save logs, and on every disconnect)
    //   muuk_reports/*.pdf              exported reports
    [[nodiscard]] static std::filesystem::path dataDirectory(const std::string& configPath);

    // Loads the history and the tail of the running log for this config and
    // keeps appending the log to its file. Cheap to call every frame: only
    // does work when the path changes (or `force`).
    void attachData(const std::string& configPath, bool force = false);

    // Fresh start: the basket history and the running log are moved to
    // archive/ folders (not deleted), and the lifetime statistics (speed,
    // basket numbering) start over. Settings, the config file, reports,
    // and anything open at the broker are left alone.
    bool resetData(const std::string& configPath);

    // All saved baskets for this config (loaded on start / reloadHistory,
    // plus every basket closed since), oldest first.
    [[nodiscard]] std::vector<muuk::ctrader_fix::BasketRecord> history() const;
    // Bumped whenever history() changes, so the UI only copies it when needed.
    [[nodiscard]] uint64_t historyRevision() const { return historyRevision_.load(); }
    void reloadHistory(const std::string& configPath);

    // Writes the PDF report; returns its path, or nullopt (see lastError()).
    std::optional<std::filesystem::path> exportReport(const std::string& configPath);
    // Writes the current log; returns its path, or nullopt.
    std::optional<std::filesystem::path> saveLog(const std::string& configPath);

private:
    bool startRunner(const std::string& configPath);
    void appendLog(const std::string& line);
    void setError(const std::string& message);
    [[nodiscard]] static std::string fileStamp();

    std::shared_ptr<muuk::ctrader_fix::NewsFeed> news_;
    bool demo_ = false;
    std::chrono::steady_clock::time_point demoStart_{};
    [[nodiscard]] muuk::ctrader_fix::LiveSnapshot demoSnapshot() const;
    std::string configPath_;  // of the current/last connection
    std::string dataAttachedFor_;
    std::ofstream liveLog_;   // guarded by logMutex_

    mutable std::mutex historyMutex_;
    std::vector<muuk::ctrader_fix::BasketRecord> history_;
    std::string historyLoadedFor_;
    std::atomic<uint64_t> historyRevision_{0};

    std::unique_ptr<muuk::ctrader_fix::LiveRunner> runner_;
    std::unique_ptr<muuk::mt5_bridge::Mt5Gateway> bridge_;
    int bridgeEaPort_ = 0;
    int bridgeFixPort_ = 0;
    // Starts (or keeps) the bridge for an MT5 config; false (with the error set) if it can't listen.
    bool ensureBridge(const muuk::ctrader_fix::LiveConfig& config);
    void launchMt5(const muuk::ctrader_fix::LiveConfig& config);
    std::thread mt5Thread_;
    std::atomic<bool> mt5Starting_{false};
    bool mt5LaunchedThisStart_ = false;
    mutable std::mutex mt5Mutex_;
    std::string mt5Message_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    // One bot per config (see InstanceLock): held from start() to stop(),
    // across reconnects.
    muuk::ctrader_fix::InstanceLock instanceLock_;

    // Auto-reconnect (GUI thread only, except the atomics).
    std::atomic<bool> wantConnected_{false};  // between start() and stop()
    std::atomic<bool> autoReconnect_{true};
    std::atomic<bool> dropped_{false};        // run() ended without stop() -- set by the runner thread
    int reconnects_ = 0;
    int consecutiveFailures_ = 0;
    bool reconnectScheduled_ = false;
    std::chrono::steady_clock::time_point reconnectAt_;
    bool stableSet_ = false;
    std::chrono::steady_clock::time_point stableSince_;
    std::string lastDrop_;
    std::string lastDropAt_;

    mutable std::mutex logMutex_;
    std::vector<std::string> log_;
    static constexpr std::size_t kMaxLogLines = 2000;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace muuk::desktop
