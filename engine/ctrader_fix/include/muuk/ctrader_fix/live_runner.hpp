#pragma once

#include <atomic>
#include <chrono>
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

// One currently open (filled) position, for a UI to list individually --
// see LiveRunner's OpenLeg comment for why this can't just be a net figure.
struct PositionInfo {
    muuk::core::OrderId id = muuk::core::kInvalidOrderId;
    muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
    double lots = 0.0;
    double entryPrice = 0.0;
    double floatingPnl = 0.0;  // $, marked at the price it would close at (bid for a buy, ask for a sell)
};

// One currently working (unfilled) order, for a UI to list individually.
struct PendingOrderInfo {
    muuk::core::OrderId id = muuk::core::kInvalidOrderId;
    muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
    double lots = 0.0;
    double price = 0.0;
};

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
    std::vector<PositionInfo> positions;
    std::vector<PendingOrderInfo> pendingOrders;

    bool connected = false;          // both FIX sessions up and the engine constructed
    bool seeding = false;            // seed orders armed, real range not anchored yet
    bool closingInProgress = false;  // closing orders sent, fills not all confirmed yet
    bool closeFailed = false;        // a closing order was rejected -- needs the operator
    int cooldownRemainingSeconds = 0;
    int basketsClosed = 0;           // this session
    double realizedPnl = 0.0;        // this session, sum of each basket's net P/L at close
    LiveSettings settings;
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

    // Manual operator controls. All are safe to call from any thread while
    // run() is active, and no-ops if it isn't (there's no session to send
    // on).
    //
    // openRange(): starts a new cycle centered on live price, using the
    // current settings. Refuses (logs why, returns false) unless the
    // account is completely flat -- no open positions, no pending orders,
    // no closing orders still in flight -- so a new range can never open
    // on top of an unfinished one.
    //
    // closeRange(): ends the running cycle -- cancels every pending order,
    // closes every open position, and leaves the engine Idle. The bot
    // won't open another range on its own until openRange() is called.
    //
    // closeAllPositions()/cancelAllPendingOrders(): each half of
    // closeRange() on its own; both also stop the cycle (engine Idle),
    // since the engine can't be told to forget only part of its state.
    bool openRange();
    void closeRange();
    void closeAllPositions();
    void cancelAllPendingOrders();

    // Current tunable settings, and a way to change them. Changes that
    // shape a cycle (range width, lots, target, ...) take effect when the
    // next range opens -- never mid-cycle; autoContinue and the cooldown
    // take effect immediately.
    [[nodiscard]] LiveSettings settings() const;
    void applySettings(const LiveSettings& settings);

    // Per-item versions of the above, for a UI to act on a single row of
    // its positions/pending-orders list, leaving the rest of the running
    // cycle untouched (unlike the "all" versions, these do NOT stop the
    // cycle -- that would also drop every other order/position the
    // strategy still has open, which defeats the point of "just this
    // one"). The engine's own P/L aggregate may run slightly stale for the
    // rest of this cycle as a result (it has no way to be told "forget
    // this one leg") -- bounded, cosmetic, and already an accepted
    // Phase-1 gap (see docs/ctrader-fix.md's note on the seed-cancel
    // race). No-op if `id` isn't currently a live position/pending order.
    void closePosition(muuk::core::OrderId id);
    void cancelOrder(muuk::core::OrderId id);

private:
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
        muuk::core::OrderId id = muuk::core::kInvalidOrderId;
        muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
        muuk::core::Volume volume;
        // The requested boundary price while still in placedOrderInfo_;
        // overwritten with the real fill price once moved into openLegs_.
        muuk::core::Price price;
    };

    void log(const std::string& line) const;

    void onQuoteMessage(const FixMessage& msg);
    void onTradeMessage(const FixMessage& msg);
    void handleCommand(const muuk::core::Command& cmd);  // caller must hold engineMutex_
    // Sends one closing Market order per leg in `legs` and tracks each by
    // ClOrdID in pendingCloseClOrdIds_. Does not touch openLegs_ itself --
    // callers pass in (and are responsible for having already cleared)
    // whatever list they want closed. Caller must hold engineMutex_.
    void closeLegs(const std::vector<OpenLeg>& legs);
    // Shared bodies of the public manual controls. Caller must hold engineMutex_.
    void cancelPendingLocked();
    void closePositionsLocked();
    // True when nothing from any cycle is still live at the broker: no open
    // position, no pending order, no closing order awaiting its fill.
    // Caller must hold engineMutex_.
    [[nodiscard]] bool accountFlat() const;
    // Hands config_.strategy (including any settings changed since the
    // last cycle) to the engine, first re-centering its range on the
    // latest mid if `recenter` and a quote is available. Only valid while
    // no cycle is running. Caller must hold engineMutex_.
    void prepareNextCycle(bool recenter);
    void resolveSymbol();
    void maybeEmitTick();  // caller must hold engineMutex_; no-op if engine_ isn't constructed yet

    // Starts the next cycle (engine_->restart()) only when the engine has
    // actually completed a basket AND every leg of that basket's real
    // position has been individually flattened at the broker
    // (pendingCloseClOrdIds_ empty again -- see the CloseBasketCommand
    // handling in handleCommand()) or never needed flattening in the first
    // place (openLegs_ was empty), AND (if connection.basketCooldownSeconds
    // > 0) that many seconds have passed since the moment it became fully
    // flat. This is what stops a new range/basket from opening while the
    // previous one's P/L is still un-booked at the broker, and then keeps
    // it from immediately re-seeding into the same brief spike that just
    // closed it. Never restarts once basketCloseFailed_ is set -- see its
    // comment. Called on every live quote tick (maybeEmitTick) as well as
    // right when the last closing leg confirms, so the cooldown elapsing
    // is noticed promptly either way. Caller must hold engineMutex_.
    void maybeRestartAfterBasketClose();

    // Blocks until the first live bid+ask arrives. Needed before the
    // engine is started whenever the first range depends on live price:
    // auto-centering (connection.autoRangeWidthPoints) and seed orders
    // (which are placed off the real bid/ask). Quote subscription happens
    // before engine construction specifically so this can observe a tick.
    void waitForFirstQuote();
    // Centers config_.strategy.range on the latest mid with
    // connection.autoRangeWidthPoints as its width. Caller must hold
    // engineMutex_ and a quote must have arrived.
    void centerRangeOnLatestQuote();
    // The latest quote as an engine tick. Caller must hold engineMutex_
    // and a quote must have arrived.
    [[nodiscard]] muuk::core::TickEvent latestTick() const;

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

    // side/volume/price for an order LiveRunner has placed but that
    // hasn't filled (or been cancelled) yet -- moved into openLegs_ on
    // fill, or into cancelledOrderInfo_ once a cancel is sent for it.
    std::unordered_map<muuk::core::OrderId, OpenLeg> placedOrderInfo_;
    std::vector<OpenLeg> openLegs_;

    // Orders LiveRunner has sent OrderCancelRequest for but hasn't yet
    // heard back about -- the broker might still confirm the fill instead
    // of the cancel (a real, observed race: a seed order this close to
    // market can fill in the same window its sibling's cancel is in
    // flight). If a Trade fill ExecutionReport later arrives for one of
    // these, it's treated as a surplus leg and closed immediately (not
    // silently folded into openLegs_ to wait out the rest of the cycle) --
    // see onTradeMessage. Moved back to placedOrderInfo_ if the broker
    // instead rejects the cancel (OrderCancelReject) -- the order is still
    // genuinely live in that case.
    std::unordered_map<muuk::core::OrderId, OpenLeg> cancelledOrderInfo_;

    // ClOrdIDs of the individual closing Market orders sent for the
    // current basket close, still awaiting a fill confirmation.
    std::unordered_set<std::string> pendingCloseClOrdIds_;
    // True once any closing leg has been rejected by the broker: the
    // position may be only partially flattened, so auto-restart is
    // permanently disabled (operator must intervene) rather than risk
    // piling a new basket on top of leftover exposure. Cleared only by
    // constructing a fresh LiveRunner.
    bool basketCloseFailed_ = false;

    // Set once (by maybeRestartAfterBasketClose) the moment a basket
    // becomes fully flat, so connection.basketCooldownSeconds is measured
    // from actual confirmed-flat time, not from whenever the engine first
    // decided to close. Cleared once the next cycle actually starts.
    bool cooldownActive_ = false;
    std::chrono::steady_clock::time_point cooldownUntil_;

    int basketsClosed_ = 0;
    double realizedPnl_ = 0.0;
    std::atomic<bool> running_{false};

    std::atomic<bool> stopping_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopped_ = false;

    LogHandler logHandler_;
};

}  // namespace muuk::ctrader_fix
