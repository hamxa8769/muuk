#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "muuk/core/strategy_engine.hpp"
#include "muuk/ctrader_fix/basket_history.hpp"
#include "muuk/ctrader_fix/fix_session.hpp"
#include "muuk/ctrader_fix/latency_stats.hpp"
#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_state.hpp"
#include "muuk/ctrader_fix/messages.hpp"

namespace muuk::ctrader_fix {

// One currently open (filled) position, for a UI to list individually --
// see LiveRunner's OpenLeg comment for why this can't just be a net figure.
// Money breakdown for one position or a whole set of them, using the same
// convention as the engine's basket P/L:
//   net = gross - slippage - spread - commission - swap
// gross      move from the price each order was *meant* to fill at to the mid
// slippage   actual fill vs. requested price (negative = a better fill)
// spread     cost of crossing to the closing side right now (mid -> bid/ask)
// commission configured round-turn rate x lots
// swap       overnight financing -- not tracked yet, always 0
// net is exactly what closing at the current bid/ask would realize after
// commission.
struct PnlBreakdown {
    double gross = 0.0;
    double slippage = 0.0;
    double spread = 0.0;
    double commission = 0.0;
    double swap = 0.0;
    double net = 0.0;

    [[nodiscard]] double costs() const { return slippage + spread + commission + swap; }
    PnlBreakdown& operator+=(const PnlBreakdown& o) {
        gross += o.gross;
        slippage += o.slippage;
        spread += o.spread;
        commission += o.commission;
        swap += o.swap;
        net += o.net;
        return *this;
    }
};

struct PositionInfo {
    muuk::core::OrderId id = muuk::core::kInvalidOrderId;  // LiveRunner's handle for closePosition()
    std::string positionId;  // the broker's position id (tag 721); empty = unknown, can't be closed by the bot
    bool adopted = false;    // found at the broker by a position sync, not opened by this session
    muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
    double lots = 0.0;
    double requestedPrice = 0.0;  // the Stop price the order was placed at
    double entryPrice = 0.0;      // the price it actually filled at
    double floatingPnl = 0.0;     // == pnl.net
    PnlBreakdown pnl;
};

// One currently working (unfilled) order, for a UI to list individually.
struct PendingOrderInfo {
    muuk::core::OrderId id = muuk::core::kInvalidOrderId;
    muuk::core::OrderSide side = muuk::core::OrderSide::BuyStop;
    double lots = 0.0;
    double price = 0.0;
    bool adopted = false;  // already working at the broker when the bot connected, not from a saved basket
    // Held by the bot, not at the broker yet: the market is already past its
    // price (a Buy Stop at or below the ask, a Sell Stop at or above the bid),
    // so the broker would refuse it. It's sent as soon as the price is back
    // on the right side -- or right away at market with placeWaitingOrderNow().
    bool waiting = false;
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
    bool syncingPositions = false;   // a Request For Positions is in flight
    int cooldownRemainingSeconds = 0;
    int basketsClosed = 0;           // this session

    PnlBreakdown live;               // all open positions, marked now (sum of positions[].pnl)
    // Closed this session, from the *actual* fill prices of both the entry
    // and the closing trade: gross = real price move, commission = rate x
    // lots, net = gross - commission. (Spread and slippage are already
    // inside real fill prices, so they aren't charged again here.)
    double bookedGross = 0.0;
    double bookedCommission = 0.0;
    double realizedPnl = 0.0;        // booked net
    LiveSettings settings;

    // Speed. tickDecision / fillReaction are the bot's own processing time
    // in microseconds (quote in -> engine decided; fill in -> reaction sent);
    // brokerRoundTrip is order sent -> broker's first answer, in ms.
    LatencySummary tickDecisionUs;
    LatencySummary fillReactionUs;
    LatencySummary brokerRoundTripMs;
    uint64_t ticksProcessed = 0;

    std::string sessionStartedAt;
    std::string statsSince;    // lifetime statistics (speed, basket numbering) count from here -- see resetStats()
    std::string resumeNote;    // what happened to the saved basket on connect (resumed / couldn't), empty if nothing

    // Connection health.
    std::string connectedSince;          // when this connection became ready ("" before)
    std::string disconnectReason;        // why the last session ended ("" while up)
    std::optional<double> clockOffsetMs; // this PC's clock minus the broker's SendingTime (includes one-way latency)
    std::vector<BasketRecord> baskets;           // completed this session, oldest first
    std::optional<BasketRecord> currentBasket;   // the basket in progress, if any
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

    // Called once for every basket as soon as it's completely closed (every
    // position's closing fill confirmed) -- e.g. to append it to a history
    // file. Runs on a session thread while LiveRunner's lock is held, so
    // keep it short and don't call back into LiveRunner. Same "set before
    // run()" rule as setLogHandler.
    using BasketHandler = std::function<void(const BasketRecord&)>;
    void setBasketHandler(BasketHandler handler) { basketHandler_ = std::move(handler); }

    // State persistence. When set (before run()), the running basket --
    // engine state, open positions, pending orders, closes in flight --
    // plus id counters and lifetime statistics are saved to this file on
    // every change. On the next connect the saved basket is checked against
    // what the broker actually has (Request For Positions + Order Mass
    // Status Request) and picked up where it left off:
    //   - positions and pending orders still there: the basket simply
    //     carries on (same lot sequence, same target);
    //   - a pending order that filled while disconnected: matched to its
    //     new position (same side and size, near the Stop price) and
    //     processed as the fill it was, so the next order goes out;
    //   - closes that were in flight: redone if the position is still open.
    // If the broker doesn't match the saved basket in a way that can be
    // explained (a position or order removed outside the bot), it is NOT
    // resumed: the bot's pending orders are cancelled, the remaining
    // positions are listed, and nothing new opens until they're closed.
    // Needs connection.syncPositionsOnConnect (the default).
    void setStateFile(std::filesystem::path file) { stateFile_ = std::move(file); }

    // Starts the lifetime statistics over: speed totals, quote count,
    // basket numbering and this session's booked results. Leaves the
    // running basket, positions and orders untouched.
    void resetStats();

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
    // closes every open position, and leaves the engine Idle. With
    // `reopen` (the default), autoContinue on and
    // connection.manualCloseCooldownSeconds > 0, a new range opens around
    // live price that many seconds after the account is confirmed flat
    // (the snapshot shows the countdown); otherwise the bot waits for
    // openRange(). reopen=false is for shutting down.
    //
    // closeAllPositions()/cancelAllPendingOrders(): each half of
    // closeRange() on its own; both also stop the cycle (engine Idle),
    // since the engine can't be told to forget only part of its state.
    bool openRange();
    void closeRange(bool reopen = true);
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

    // Sends a waiting order (see PendingOrderInfo::waiting) right now as a
    // market order instead of waiting for the price to come back. It then
    // counts as that order's fill, at whatever price the market gives.
    void placeWaitingOrderNow(muuk::core::OrderId id);

    // Asks the broker for every open position (Request For Positions) and,
    // when the answer arrives, reconciles the tracked positions with it:
    // positions on this symbol that the bot didn't know about (e.g. left
    // from an earlier run) are adopted so they can be closed from here, and
    // tracked positions the broker no longer has (closed in cTrader by
    // hand) are dropped. Asynchronous; no-op if one is already in flight.
    void syncPositions();

    // Exit guard: cancels every pending order, closes every open position
    // (like closeRange(), so no new range opens afterwards either) and then
    // blocks until the broker has confirmed the account is flat, a close is
    // rejected, the connection drops, or `timeout` passes. Returns true only
    // once the account is confirmed flat (immediately if it already is).
    // Call from any thread other than the one running run().
    bool flattenAndWait(std::chrono::milliseconds timeout);

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
        // Always the requested boundary price (for slippage).
        muuk::core::Price requestedPrice;
        // The broker's position id (tag 721 of the opening fill). Closing
        // orders must carry it -- see buildMarketOrderSingle.
        std::string positionId;
        bool adopted = false;  // found by a position sync rather than opened this session
    };

    // Applies a complete set of Position Reports (see syncPositions()).
    // Caller must hold engineMutex_.
    void finishPositionSync(const std::vector<PositionReportInfo>& reports);
    // Order Mass Status Request round trip (connect only). Caller must hold engineMutex_.
    void requestOrderStatusLocked();
    void finishOrderSync(std::vector<ExecutionReportInfo> reports, bool known);

    // A fill for one of the bot's orders: a placed order becomes an open
    // leg and the engine reacts (next order); one whose cancel was already
    // sent is closed straight away as surplus. Shared by live fills and
    // fills found on reconnect. Caller must hold engineMutex_.
    void applyFillLocked(muuk::core::OrderId orderId, muuk::core::Price fillPrice, const std::string& positionId);

    // State persistence (see setStateFile). loadSavedState: reads the file
    // (before connecting). reconcileOnConnectLocked: matches it against the
    // broker's positions/orders and resumes or safely winds down; engine_
    // must exist. buildStateLocked: the current state. flushState: writes
    // it if anything changed (never before reconcile has run, so a saved
    // basket can't be overwritten by an empty one). markDirty: flags a change.
    void loadSavedState();
    void reconcileOnConnectLocked();
    // Positions found at the broker with no saved basket (or only as "old"
    // ones) that look exactly like one of the bot's baskets -- lots 0.01,
    // 0.02, 0.03... with alternating sides -- are picked up as a running
    // basket: range from the first fill, the engine rebuilt, and the next
    // order armed. false (with the reason) if they don't fit.
    bool adoptBasketFromBrokerLocked(std::string& why);
    // Stop orders the market has already passed are held back rather than
    // sent (the broker would refuse them); this sends the ones that have
    // become valid. Caller must hold engineMutex_.
    [[nodiscard]] bool stopIsMarketable(muuk::core::OrderSide side, muuk::core::Price price) const;
    void sendStopOrderLocked(const OpenLeg& leg, int sequenceIndex);
    void releaseWaitingOrdersLocked();
    // Watchdog, run about once a second: a running basket must always
    // have its next order at the broker (or waiting). An order the engine
    // counts on that the bot doesn't have is placed again; a basket with
    // positions and no pending order at all gets its next one. Both are
    // logged as MISSED. Caller must hold engineMutex_.
    void checkBasketHealthLocked();
    [[nodiscard]] PersistedState buildStateLocked() const;
    void flushState();
    void markDirty() { stateDirty_ = true; }

    // Basket recording (all: caller must hold engineMutex_).
    // beginBasket: a new range has just been armed.
    // recordLegOpened/Closed: a position of the current basket opened/closed.
    // maybeFinalizeBasket: once nothing of the basket is left open at the
    // broker and no range is running, stamps it closed, keeps it in
    // completedBaskets_ and hands it to basketHandler_.
    void beginBasket();
    void recordLegOpened(const OpenLeg& leg);
    void recordLegClosed(const OpenLeg& leg, double exitPrice, double gross, double commission,
                         const std::string& note = {});
    void setCloseReason(const std::string& reason);
    void maybeFinalizeBasket();

    // Live breakdown for one open leg at the latest quote. Caller must hold
    // engineMutex_ and a quote must have arrived.
    [[nodiscard]] PnlBreakdown legPnl(const OpenLeg& leg) const;

    void log(const std::string& line) const;

    void onQuoteMessage(const FixMessage& msg);
    void onTradeMessage(const FixMessage& msg);
    void handleCommand(const muuk::core::Command& cmd);  // caller must hold engineMutex_
    // Sends one closing Market order per leg in `legs` and tracks each by
    // ClOrdID in pendingCloses_. Does not touch openLegs_ itself --
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
    // (pendingCloses_ empty again -- see the CloseBasketCommand
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

    // Orders the engine armed that the bot is holding back because the
    // market is already past their price -- see PendingOrderInfo::waiting.
    std::unordered_map<muuk::core::OrderId, OpenLeg> waitingOrders_;
    int healthRearms_ = 0;
    // Orders the operator cancelled one by one (cancelOrder): the watchdog
    // leaves them cancelled.
    std::unordered_set<muuk::core::OrderId> operatorCancelled_;  // watchdog re-arms since the last fill -- capped, so a rejecting broker isn't spammed

    // Closing Market orders sent and still awaiting a fill confirmation,
    // keyed by their ClOrdID, each with the leg it closes -- so the actual
    // realized P/L can be computed from the real exit fill.
    std::unordered_map<std::string, OpenLeg> pendingCloses_;
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
    double bookedGross_ = 0.0;
    double bookedCommission_ = 0.0;

    // Position sync (Request For Positions / Position Report) state.
    std::string activePosReqId_;  // non-empty while a request is in flight
    uint64_t nextPosReqId_ = 1;
    std::vector<PositionReportInfo> posReports_;
    std::unordered_set<std::string> knownAtPosRequest_;  // position ids tracked when the request was sent
    std::unordered_set<std::string> closedPositionIds_;  // closed by the bot -- never re-adopt from a stale report
    muuk::core::OrderId nextAdoptedId_ = 1'000'000'000;  // handles for adopted positions, far from engine order ids
    std::mutex posSyncMutex_;
    std::condition_variable posSyncCv_;
    bool posSyncDone_ = false;
    bool orderSyncDone_ = false;  // guarded by posSyncMutex_

    // Connect-time sync: while connectSync_ is set, position/order answers
    // are only collected here, for reconcileOnConnectLocked().
    bool connectSync_ = false;
    bool connectPositionsKnown_ = false;
    std::vector<PositionReportInfo> connectPositions_;
    bool connectOrdersKnown_ = false;
    std::vector<ExecutionReportInfo> connectOrders_;
    std::string activeMassStatusReqId_;
    std::vector<ExecutionReportInfo> orderReports_;
    uint64_t nextMassStatusReqId_ = 1;

    // Persistence.
    std::filesystem::path stateFile_;
    std::optional<PersistedState> loadedState_;
    std::atomic<bool> stateDirty_{false};
    bool stateReady_ = false;      // reconcile done -- saving allowed
    std::mutex stateWriteMutex_;   // one writer at a time, newest wins
    uint64_t stateSeq_ = 0;        // guarded by engineMutex_
    uint64_t lastWrittenSeq_ = 0;  // guarded by stateWriteMutex_
    std::string statsSince_;
    std::string resumeNote_;
    bool holdAutoStart_ = false;

    // Connection health (see LiveSnapshot). infoMutex_ rather than
    // engineMutex_: disconnect handlers can run while a send holds the latter.
    mutable std::mutex infoMutex_;
    std::string connectedSince_;
    std::string disconnectReason_;
    std::atomic<double> clockOffsetMs_{0.0};
    std::atomic<bool> clockOffsetKnown_{false};
    uint64_t quoteMessages_ = 0;  // guarded by engineMutex_
    uint64_t statusTicks_ = 0;    // ticksProcessed_ at the last [status] line
    // Close range asked for a new range after connection.manualCloseCooldownSeconds.
    bool reopenAfterManualClose_ = false;  // a saved basket couldn't be resumed: the operator decides what's next

    // Basket history.
    std::string sessionStartedAt_;
    int basketCounter_ = 0;
    std::optional<BasketRecord> currentBasket_;
    std::chrono::steady_clock::time_point currentBasketStart_;
    std::string closeReason_;
    std::vector<BasketRecord> completedBaskets_;
    BasketHandler basketHandler_;

    // Speed.
    LatencyTracker tickDecisionUs_;
    LatencyTracker fillReactionUs_;
    LatencyTracker brokerRoundTripMs_;
    uint64_t ticksProcessed_ = 0;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> orderSentAt_;  // by ClOrdID
    std::atomic<bool> running_{false};

    std::atomic<bool> stopping_{false};
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    bool stopped_ = false;

    LogHandler logHandler_;
};

}  // namespace muuk::ctrader_fix
