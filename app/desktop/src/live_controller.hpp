#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"

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
    // call even if not running.
    void stop();

    // Range / position controls -- forwarded straight to LiveRunner (see
    // its comments for exactly what each does). No-ops if not running.
    bool openRange();
    void closeRange();
    void closeAllPositions();
    void cancelAllPendingOrders();
    void closePosition(muuk::core::OrderId id);
    void cancelOrder(muuk::core::OrderId id);
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

    [[nodiscard]] bool isRunning() const { return running_.load(); }
    // Returns the last known state even after the runner has stopped
    // (e.g. a dropped connection), so the UI can show what happened
    // rather than blanking out. std::nullopt only before the first
    // successful start().
    [[nodiscard]] std::optional<muuk::ctrader_fix::LiveSnapshot> snapshot() const;
    [[nodiscard]] std::vector<std::string> logLines() const;
    void clearLog();
    [[nodiscard]] std::string lastError() const;
    void clearError();

    // --- History, reports, logs -------------------------------------------
    // Everything is stored next to the config file:
    //   muuk_history/baskets.jsonl   every closed basket (appended as it closes)
    //   muuk_reports/*.pdf           exported reports
    //   muuk_logs/*.log              saved logs (also written automatically on disconnect)
    [[nodiscard]] static std::filesystem::path dataDirectory(const std::string& configPath);

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
    void appendLog(const std::string& line);
    void setError(const std::string& message);
    [[nodiscard]] static std::string fileStamp();

    std::string configPath_;  // of the current/last connection

    mutable std::mutex historyMutex_;
    std::vector<muuk::ctrader_fix::BasketRecord> history_;
    std::string historyLoadedFor_;
    std::atomic<uint64_t> historyRevision_{0};

    std::unique_ptr<muuk::ctrader_fix::LiveRunner> runner_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex logMutex_;
    std::vector<std::string> log_;
    static constexpr std::size_t kMaxLogLines = 2000;

    mutable std::mutex errorMutex_;
    std::string lastError_;
};

}  // namespace muuk::desktop
