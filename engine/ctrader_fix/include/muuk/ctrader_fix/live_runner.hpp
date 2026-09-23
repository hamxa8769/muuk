#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    void maybeEmitTick();  // caller must hold engineMutex_; no-op if engine_ isn't constructed yet

    // Starts the next cycle (engine_->restart()) only when the engine has
    // actually completed a basket AND every leg of that basket's real
    // position has been individually flattened at the broker
    // (pendingCloseClOrdIds_ empty again -- see the CloseBasketCommand
    // handling in handleCommand()) or never needed flattening in the first
    // place (openLegs_ was empty). This is what stops a new range/basket
    // from opening while the previous one's P/L is still un-booked at the
    // broker. Never restarts once basketCloseFailed_ is set -- see its
    // comment. Caller must hold engineMutex_.
    void maybeRestartAfterBasketClose();

    // Blocks until the first live bid+ask arrives, then centers
    // config_.strategy.range around its midpoint using
    // connection.autoRangeWidthPoints. Only called when that width is >0.
    // Must run before engine_ is constructed (see engine_'s comment) —
    // quote subscription happens before that construction specifically so
    // this can observe a real tick first.
    void waitForFirstQuoteAndCenterRange();

    LiveConfig config_;
    std::string symbolId_;

    std::unique_ptr<FixSession> quoteSession_;
    std::unique_ptr<FixSession> tradeSession_;

    mutable std::mutex engineMutex_;
    // Constructed in run() — after symbol resolution and (if
    // autoRangeWidthPoints>0) range auto-centering are both done, not in
    // this class's constructor — because StrategyEngine takes a copy of
    // StrategyConfig once and never re-reads config_.strategy afterward,
    // so config_.strategy.range must already be final by the time this is
    // built. Null until then; onQuoteMessage/maybeEmitTick handle that
    // (ticks may start arriving, for auto-centering, before it exists).
    std::unique_ptr<muuk::core::StrategyEngine> engine_;
    std::optional<double> latestBid_;
    std::optional<double> latestAsk_;

    std::mutex symbolMutex_;
    std::condition_variable symbolCv_;
    bool symbolResolved_ = false;

    std::mutex firstQuoteMutex_;
    std::condition_variable firstQuoteCv_;
    bool firstQuoteReceived_ = false;

    std::atomic<uint64_t> nextCancelReqId_{1};
    std::atomic<uint64_t> nextCloseReqId_{1};

    // Per-leg position tracking. engine/core deliberately only keeps a
    // running (side, volume) *aggregate* per side (see SideAggregate in
    // strategy_engine.cpp) -- fixed-size by design, not a list of
    // individual fills. That's fine for its own P/L math, but it means a
    // live driver can't rely on the engine to know *how many separate
    // broker positions* are open, only the net exposure. That distinction
    // matters here: on a hedging-enabled account (this demo account is
    // one -- cTrader keeps every fill as its own position rather than
    // merging same-symbol exposure), a single net-sized closing order does
    // NOT close out several smaller same-symbol positions, even when their
    // net volume is zero (equal Buy and Sell fills can leave the net
    // exposure at 0 while every individual position is still open and
    // un-booked). So LiveRunner tracks each real fill as its own leg here
    // and closes every leg individually on CloseBasketCommand, instead of
    // trusting CloseBasketCommand's own closeSide/closeVolume (which is
    // still correct as the *net* figure, just not sufficient on its own
    // for a hedging account).
    struct OpenLeg {
        muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
        muuk::core::Volume volume;
    };
    // side/volume for an order LiveRunner has placed but that hasn't
    // filled (or been cancelled) yet -- moved into openLegs_ on fill.
    std::unordered_map<muuk::core::OrderId, OpenLeg> placedOrderInfo_;
    std::vector<OpenLeg> openLegs_;

    // ClOrdIDs of the individual closing Market orders sent for the
    // current basket close, still awaiting a fill confirmation.
    std::unordered_set<std::string> pendingCloseClOrdIds_;
    // True once any closing leg has been rejected by the broker: the
    // position may be only partially flattened, so auto-restart is
    // permanently disabled (operator must intervene) rather than risk
    // piling a new basket on top of leftover exposure. Cleared only by
    // constructing a fresh LiveRunner.
    bool basketCloseFailed_ = false;
    std::atomic<bool> running_{false};

    std::atomic<bool> stopping_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopped_ = false;

    LogHandler logHandler_;
};

}  // namespace muuk::ctrader_fix
