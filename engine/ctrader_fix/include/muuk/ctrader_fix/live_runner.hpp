#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "muuk/core/strategy_engine.hpp"
#include "muuk/ctrader_fix/fix_session.hpp"
#include "muuk/ctrader_fix/live_config.hpp"

namespace muuk::ctrader_fix {

// A point-in-time read of LiveRunner's state, for a UI (or anything else)
// to poll without needing its own hooks into the engine internals. Built
// from the same data onQuoteMessage/onTradeMessage already maintain, so
// there's no separate state to keep in sync.
struct LiveSnapshot {
    muuk::core::EngineStatus status = muuk::core::EngineStatus::Idle;
    std::string symbolName;
    std::string symbolId;
    double rangeLower = 0.0;
    double rangeUpper = 0.0;
    int activeOrderCount = 0;
    double lastGrossPnlMoney = 0.0;
    double lastNetPnlMoney = 0.0;
    std::optional<double> bid;
    std::optional<double> ask;
};

// The live counterpart to backtester::BacktestRunner
// (engine/backtester/include/muuk/backtester/backtest_runner.hpp):
// connects two FixSessions (Quote, Trade), resolves the configured symbol
// name to cTrader's numeric id, subscribes to top-of-book quotes, and
// drives the exact same muuk::core::StrategyEngine — translating its
// commands into real NewOrderSingle/OrderCancelRequest messages and
// feeding broker ExecutionReports back in as OrderConfirmedEvent/
// OrderRejectedEvent. engine/core is not modified or even aware this
// driver exists.
//
// Engine access is guarded by one mutex because the Quote and Trade
// sessions each run their own reader thread and both can call into the
// engine concurrently (quotes drive onTick, trade fills drive
// onOrderConfirmed/onOrderRejected).
class LiveRunner {
public:
    using LogHandler = std::function<void(const std::string&)>;

    explicit LiveRunner(LiveConfig config);
    ~LiveRunner();

    LiveRunner(const LiveRunner&) = delete;
    LiveRunner& operator=(const LiveRunner&) = delete;

    // Receives every status/order/error line this class would otherwise
    // print to stderr (unchanged default when no handler is set — the CLI
    // relies on that). May be called from any thread that calls into
    // LiveRunner. Must be set before run() — like FixSession's
    // setDisconnectHandler, it is read without synchronization once
    // running.
    void setLogHandler(LogHandler handler) { logHandler_ = std::move(handler); }

    // Thread-safe point-in-time read of the current state, for a UI to
    // poll. Safe to call from any thread, including while run() is active
    // on another one.
    [[nodiscard]] LiveSnapshot snapshot() const;

    // Connects both sessions, resolves the symbol, arms the engine's
    // initial orders, subscribes to quotes, and then blocks (processing
    // happens on the sessions' own reader threads) until stop() is called
    // or a session disconnects. Always tears both sessions down via
    // stop() before returning/throwing — including on a startup failure
    // (resolve/connect/logon/symbol-not-found), so a caller that catches
    // the exception is never left with sessions still connected in the
    // background.
    void run();

    // Thread-safe, and safe to call concurrently with run()'s own
    // teardown (which also calls this) or from multiple threads at once —
    // only the first caller actually tears the sessions down; the rest
    // block until it's done rather than racing on the same FixSession
    // objects (a prior version of this raced there intermittently).
    void stop();

private:
    void log(const std::string& line) const;

    void onQuoteMessage(const FixMessage& msg);
    void onTradeMessage(const FixMessage& msg);
    void handleCommand(const muuk::core::Command& cmd);  // caller must hold engineMutex_
    void resolveSymbol();
    void maybeEmitTick();  // caller must hold engineMutex_

    LiveConfig config_;
    std::string symbolId_;

    std::unique_ptr<FixSession> quoteSession_;
    std::unique_ptr<FixSession> tradeSession_;

    mutable std::mutex engineMutex_;
    muuk::core::StrategyEngine engine_;
    std::optional<double> latestBid_;
    std::optional<double> latestAsk_;

    std::mutex symbolMutex_;
    std::condition_variable symbolCv_;
    bool symbolResolved_ = false;

    std::atomic<uint64_t> nextCancelReqId_{1};
    std::atomic<bool> running_{false};

    std::atomic<bool> stopping_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopped_ = false;

    LogHandler logHandler_;
};

}  // namespace muuk::ctrader_fix
