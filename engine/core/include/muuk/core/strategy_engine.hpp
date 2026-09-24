#pragma once

#include <optional>

#include "muuk/core/events.hpp"
#include "muuk/core/fixed_vector.hpp"
#include "muuk/core/lot_progression.hpp"
#include "muuk/core/orders.hpp"
#include "muuk/core/pnl.hpp"
#include "muuk/core/risk.hpp"
#include "muuk/core/types.hpp"

namespace muuk::core {

enum class EngineStatus {
    Idle,          // constructed, start() not called yet
    Active,        // one or more pending orders armed / legs open
    HaltedByRisk,  // a hard risk limit was hit; no new orders will be placed
    Completed,     // basket profit target reached and closed this cycle
};

// Which order(s) a new cycle starts with.
enum class StartSide {
    Both,      // a Buy Stop and a Sell Stop at once (proposal §3's worked example)
    BuyOnly,   // a single Buy Stop; the Sell Stop only follows once it fills
    SellOnly,  // a single Sell Stop; the Buy Stop only follows once it fills
};

struct StrategyConfig {
    SymbolSpec symbol;
    RangeConfig range;
    LotProgressionConfig lotProgression;
    RiskLimits riskLimits;
    CostInputs costs;
    double basketProfitTargetMoney = 0.0;
    // >0: close the whole basket once its net P/L falls to -this (a hard
    // loss cap; closes exactly like the profit target does). 0 = off.
    double basketStopLossMoney = 0.0;
    // Auto profit target: instead of basketProfitTargetMoney, book the basket
    // once its net profit reaches autoTargetPercent % of what the open
    // positions cost to trade (commission + the full spread, in and out).
    // So one 0.01 lot aims low, and the target grows with every fill.
    bool autoProfitTarget = false;
    double autoTargetPercent = 100.0;
    int maxRejectRetries = 3;
    // true (default): proposal §4 behavior — on fill, immediately re-arm
    // the triggering side with the next lot in the progression
    // (pyramiding into a confirmed breakout direction).
    // false: take the single fill and stop — do not re-arm that side.
    // The opposite side's still-pending order is untouched either way; a
    // breach of the basket profit target still closes everything as usual.
    bool reArmOnFill = true;

    // 0 (default): start()/restart() arm both full-width boundary orders
    // immediately, at range.upperBound/lowerBound exactly as configured.
    //
    // >0: "seed anchor" mode. `range` may already be stale by the time
    // both boundary orders would be armed (e.g. it was centered on a
    // quote a moment ago and price has since moved) — instead,
    // start()/restart() first arm two tiny seed orders this many price
    // units above/below range's midpoint. Whichever fills first proves a
    // real, currently-tradeable price; the other seed is cancelled, and
    // the real two-sided range (same total width as configured) is then
    // anchored to that confirmed fill — the filled side's own price
    // becomes one boundary, the opposite boundary is fillPrice +/- the
    // configured width — before falling through to the normal re-arm rule
    // below (StrategyConfig::reArmOnFill) exactly as any other fill would.
    double seedOffsetPoints = 0.0;

    // Both (default): start()/restart() arm a Buy Stop and a Sell Stop (or,
    // with seedOffsetPoints>0, a Buy seed and a Sell seed).
    // BuyOnly/SellOnly: arm just that one order. Nothing else is armed until
    // it fills; from then on the normal opposite-side rule applies, so there
    // is exactly one pending order at any time from the very start. With
    // seeding, the single seed's fill anchors the range the same way.
    StartSide startSide = StartSide::Both;
};

// Everything a cycle in progress needs to carry on in a new process
// (StrategyEngine::exportState/restoreState): which orders are armed,
// which fills make up the basket, where the sequence stands. Plain data,
// so a live driver can save it to disk and resume the same basket after a
// restart or reconnect instead of losing it.
struct EngineState {
    struct Armed {
        PendingOrder order;
        int retryCount = 0;
    };
    EngineStatus status = EngineStatus::Idle;
    RangeConfig range{};  // the range in force (seed mode re-anchors it)
    FixedVector<Armed, kMaxTrackedOrders> pendingOrders;
    int64_t buyVolumeSteps = 0;
    int64_t buySumEntryPriceTicksVolume = 0;
    int64_t sellVolumeSteps = 0;
    int64_t sellSumEntryPriceTicksVolume = 0;
    int nextSequenceIndex = 1;
    OrderId nextOrderId = 1;
    bool seeding = false;
    double slippageMoneyAccrued = 0.0;
    double swapMoneyAccrued = 0.0;
    // Round-turn commission of every position filled so far (worked out
    // at each fill price). <0 = unknown (an older state file): estimated
    // from the average entry prices on restore.
    double commissionAccrued = -1.0;
};

// Deterministic, I/O-free strategy state machine implementing the
// range-based pending-order grid described in the proposal (§3-§8):
//   - two boundary pending orders (Buy Stop at range.upperBound,
//     Sell Stop at range.lowerBound), lot size from the global
//     progression L(n) = L0 + (n-1)*deltaL (proposal §3)
//   - by default (StrategyConfig::reArmOnFill), a fill immediately arms a
//     new pending order — using the next lot in the global sequence
//     L(n) — on the *opposite* side, never re-arming the side that just
//     filled, and only if that opposite side doesn't already have one
//     pending (right after the initial pair, it does: the untouched one
//     already satisfies this). This keeps exactly one order pending at a
//     time in steady state, alternating sides on every fill, and
//     reproduces the worked example in proposal §3 exactly (Buy Stop
//     fills -> the already-pending Sell Stop is left alone -> *that*
//     fills -> Buy Stop gets a fresh order -> ...). Set reArmOnFill=false
//     to take the one fill (or two, if both sides of the initial pair
//     happen to fill) and stop there instead of continuing the sequence
//   - basket P/L is evaluated on every tick; once the net P/L target is
//     reached the whole basket is closed and remaining orders cancelled
//     (proposal §6)
//   - a risk check runs before every new order is armed; a breached limit
//     halts new order creation and raises an alert (proposal §8)
//
// The engine takes no I/O actions itself: it only consumes events and
// produces commands for the caller (backtester or live broker adapter) to
// execute. This keeps it allocation-free, deterministic, and identically
// testable in both backtest and live contexts.
class StrategyEngine {
public:
    explicit StrategyEngine(StrategyConfig config);

    // Arms the two initial boundary orders. Must be called once before any
    // onTick/onOrderConfirmed/onOrderRejected call.
    CommandBatch start();

    // Re-arms the two initial boundary orders for a new cycle. Only valid
    // when status() == Completed. Resets the sequence counter and P/L
    // accumulators; risk-limit state (HaltedByRisk) is not auto-cleared by
    // design — that requires the caller to construct a fresh engine or
    // explicitly decide to retry.
    CommandBatch restart();

    // Abandons the current cycle without emitting any commands: forgets
    // pending orders and P/L aggregates and returns to Idle (also clears
    // HaltedByRisk), so start() can arm a fresh cycle later. For a driver
    // that has already dealt with the broker-side orders/positions itself,
    // e.g. an operator's manual "close range". Keeps the config, the last
    // tick and order-id allocation, so ids stay unique across cycles.
    void stopCycle();

    // Replaces the config used by the next start()/restart(). Only valid
    // while no cycle is running (Idle or Completed).
    void reconfigure(const StrategyConfig& config);
    // Profit target / stop loss / auto target from `config`, applied right
    // away -- also in the middle of a running basket.
    void updateExitRules(const StrategyConfig& config);
    // The profit target the next tick would use (see StrategyConfig::autoProfitTarget).
    [[nodiscard]] double effectiveTargetMoney(const BasketPnlResult& pnl) const;

    // Evaluates the current basket against the profit target and the risk
    // engine against live spread. Also updates the mark price used for
    // floating P/L.
    CommandBatch onTick(const TickEvent& tick);

    // A previously placed order was filled by the broker at `fillPrice`.
    // Arms the *opposite* side with the next lot in the sequence — unless
    // it already has an order pending, or StrategyConfig::reArmOnFill is
    // false. See the class comment for why it's the opposite side, not
    // the one that just filled.
    CommandBatch onOrderConfirmed(const OrderConfirmedEvent& event);

    // A previously placed order was rejected. Retries a bounded number of
    // times (proposal §4: "controlled retry logic"); gives up silently
    // (leaving that side un-armed until the next restart) once
    // maxRejectRetries is exhausted.
    CommandBatch onOrderRejected(const OrderRejectedEvent& event);

    [[nodiscard]] EngineStatus status() const { return status_; }
    [[nodiscard]] int activeOrderCount() const { return static_cast<int>(pendingOrders_.size()); }
    [[nodiscard]] int nextSequenceIndex() const { return nextSequenceIndex_; }
    [[nodiscard]] BasketPnlResult lastBasketPnl() const { return lastBasketPnl_; }
    // The range currently in force -- differs from the configured one once
    // seed-anchor mode has re-derived it from a real fill.
    [[nodiscard]] RangeConfig currentRange() const { return config_.range; }
    [[nodiscard]] bool seeding() const { return seeding_; }

    // Snapshot of the cycle in progress, and the reverse: restoreState()
    // puts an engine (built from the same config) back exactly where
    // exportState() left it -- status, range, armed orders, basket
    // aggregates, sequence and order-id counters (an Idle state only
    // carries the order-id counter forward). It emits no commands:
    // the caller is responsible for the broker already matching the state
    // (the orders really are still pending, the fills really are open).
    [[nodiscard]] EngineState exportState() const;
    void restoreState(const EngineState& state);

    // For a driver resuming a basket whose next order is missing at the
    // broker. armNextAfter: arms what onOrderConfirmed would have armed
    // after a fill on `lastFilled` -- the next lot of the sequence on the
    // opposite side -- unless that side already has an order pending.
    // replaceOrder: drops pending order `id` and arms a fresh one in its
    // place (same side, price and place in the sequence, new id). Both only
    // act while Active, and emit the PlaceOrderCommand to send.
    CommandBatch armNextAfter(OrderSide lastFilled);
    CommandBatch replaceOrder(OrderId id);

private:
    // Running per-side accumulator backing aggregatePositionPnlMoney() —
    // deliberately not a list of individual fills: a basket can accumulate
    // an unbounded number of fills over a long trend before its profit
    // target is reached (or if no target is set), and this state must stay
    // fixed-size regardless. See pnl.hpp for the math.
    struct SideAggregate {
        int64_t totalVolumeSteps = 0;
        int64_t sumEntryPriceTicksVolume = 0;
    };

    // A pending order plus the bookkeeping needed for bounded reject-retry
    // (proposal §4). Kept private to the engine — callers only ever see the
    // plain PendingOrder/commands types.
    struct ArmedOrder {
        PendingOrder order;
        int retryCount = 0;
    };

    [[nodiscard]] Price boundaryPriceForSide(OrderSide side) const;
    [[nodiscard]] OrderId allocateOrderId() { return nextOrderId_++; }
    void resetCycleState();
    CommandBatch armInitialOrders();

    // Attempts to arm a new pending order for `side` at sequence `seqIndex`.
    // Runs the risk check first; on breach, emits a RiskAlertCommand,
    // transitions to HaltedByRisk, and returns without arming anything.
    // `priceOverride`, when set, is used instead of
    // boundaryPriceForSide(side) — only seed orders (see seedOffsetPoints)
    // need this; every normal call omits it.
    // `initial`: one of a new cycle's first orders. Only those are held back
    // by the spread/slippage limits -- once a basket is running, its next
    // order must always go out (a spread spike at that moment used to halt
    // the whole basket, leaving its positions without their next order).
    void tryArmOrder(CommandBatch& out, OrderSide side, int seqIndex, int retryCount = 0,
                      std::optional<Price> priceOverride = std::nullopt, bool initial = false);

    BasketPnlResult evaluateBasketPnl(const TickEvent& tick) const;

    StrategyConfig config_;
    EngineStatus status_ = EngineStatus::Idle;

    FixedVector<ArmedOrder, kMaxTrackedOrders> pendingOrders_;
    SideAggregate buyAggregate_;
    SideAggregate sellAggregate_;

    int nextSequenceIndex_ = 1;
    OrderId nextOrderId_ = 1;
    bool seeding_ = false;  // true between armInitialOrders() arming seeds and the first fill anchoring the range
    TickEvent lastTick_{};
    double slippageMoneyAccrued_ = 0.0;
    double swapMoneyAccrued_ = 0.0;
    double commissionAccrued_ = 0.0;
    BasketPnlResult lastBasketPnl_{};
};

}  // namespace muuk::core
