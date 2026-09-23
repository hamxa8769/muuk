# Muuk — Core Engine Architecture (Phase 1)

This covers `engine/core` (the strategy state machine) and `engine/backtester`
(the historical replay driver). See the proposal PDF at the repo root for
the full product vision; this document only covers what exists so far.

## Module boundaries

```
engine/core        <- pure logic, zero I/O, zero heap allocation on the hot path
engine/backtester   <- I/O + orchestration: reads ticks, drives engine/core,
                       simulates broker fills, aggregates statistics
```

`engine/core` never touches a file, a socket, or the clock. Everything it
needs (prices, symbol spec, risk limits) is passed in by the caller, and
everything it decides comes back out as data (a `CommandBatch`), never as a
direct side effect. This is what lets `engine/backtester` and a future live
broker adapter drive the *exact same* decision logic — the only thing that
changes between backtest and live is who calls `StrategyEngine` and how its
commands get executed.

## Event flow

```
                 +-------------------+
 TickEvent  ---> |                   | ---> PlaceOrderCommand
 OrderConfirmed->|   StrategyEngine  | ---> CancelOrderCommand
 OrderRejected ->|                   | ---> CloseBasketCommand
                 +-------------------+ ---> RiskAlertCommand
```

- `start()` arms the two boundary pending orders (Buy Stop at the range's
  upper bound, Sell Stop at the lower bound), per proposal §3 — unless
  `StrategyConfig::seedOffsetPoints > 0`, in which case it arms two tiny
  seed orders around the range's midpoint instead, and the real two-sided
  range is only established once one of them actually fills (see "Seed
  anchor mode" below). `restart()` (called after a basket closes, e.g. by
  `LiveRunner::maybeEmitTick` to keep the strategy running continuously)
  goes through the exact same logic for a new cycle.
- On `onOrderConfirmed`, when `StrategyConfig::reArmOnFill` is true
  (default), a fill immediately arms a new order — next lot in the global
  progression `L(n) = L0 + (n-1)*deltaL` — on the **opposite** side, never
  re-arming the side that just filled, and only if that opposite side
  doesn't already have one pending (right after the initial pair it does:
  the untouched one already covers it). Net effect: exactly one order
  pending at a time in steady state, alternating sides every fill — Buy
  Stop fills, the already-pending Sell Stop is left alone, *that* fills,
  Buy Stop gets a fresh order, and so on. This reproduces the worked
  example in proposal §3 exactly (see
  `engine/core/tests/test_strategy_engine.cpp`). An earlier version of
  this re-armed the *same* side that filled instead — which let one side
  accumulate unbounded fills whenever price kept moving one direction,
  since it never needed the other side to fill first; live testing
  surfaced this as "bulk orders on one side" before it was corrected.
  Setting `reArmOnFill=false` takes whichever fill(s) happen and stops
  there instead of continuing the alternating sequence; the basket-close/
  risk logic below is unaffected either way.
- On `onOrderRejected`, the engine retries the same order (same price, same
  lot) up to `StrategyConfig::maxRejectRetries` times before giving up on
  that side for the current cycle (proposal §4: "controlled retry logic").

**Seed anchor mode** (`StrategyConfig::seedOffsetPoints > 0`) exists
because a pre-computed range can already be stale by the time it's armed
— even one just centered on a live quote a moment ago, if the market is
moving fast. Instead of committing to both full-width boundaries up
front, `armInitialOrders()` arms two tiny orders -- a Buy Stop
`seedOffsetPoints` above the ask and a Sell Stop `seedOffsetPoints` below
the bid -- and sets an internal `seeding_` flag. The offsets are measured
from the side of the book each Stop order triggers on, not from the mid:
measured from the mid, any offset smaller than half the spread (0.05 vs
XAUUSD's ~0.11 spread, in live testing) put both seeds inside the spread,
so both triggered at once and opened two positions. Both seeds share a single slot in the lot progression
(`nextSequenceIndex_` advances once for the pair, not once each) — only
one of them ever actually becomes a trade, the other is always cancelled
unfilled below, so only one slot should ever be spent on it. Advancing it
twice used to make the first real re-armed order jump straight from L(1)
to L(3) (e.g. 0.01 then 0.03), silently skipping L(2) every single cycle.
Whichever seed a real fill confirms first proves an actually-tradeable
price *right now*; `onOrderConfirmed` then cancels the other seed and
re-derives the full range from that confirmed fill (the filled side's own
price becomes one boundary, the opposite boundary is `fillPrice ± the
originally configured total width`), then falls through to the normal
re-arm rule above — so only the opposite side gets a fresh order at this
point, exactly as any other fill would produce; the seed's own side isn't
re-armed until its opposite eventually fills too. The reference midpoint
itself prefers `lastTick_`
(the engine's own most recently processed tick, which persists across
`resetCycleState()`) over `range`'s configured midpoint whenever one
exists — so a `restart()` after a closed basket re-anchors to the tick
that *just* triggered the close, not to the previous (now stale) cycle's
range. Only `range`'s midpoint is used before the very first tick ever,
and then without any spread information -- so a live driver should pass
the engine a real tick before `start()` (an Idle engine's `onTick()` only
records it). `LiveRunner::run()` does exactly that.

`stopCycle()` abandons a cycle without emitting commands (back to Idle,
also clearing HaltedByRisk) for a driver that has already dealt with the
broker side itself, and `reconfigure()` swaps in new settings between
cycles. Both keep order-id allocation running, so ClOrdIDs never repeat
within a session -- which is why `LiveRunner`'s manual controls use them
rather than rebuilding the engine.
- On every `onTick`, the basket's net P/L is re-evaluated; once it reaches
  `basketProfitTargetMoney` the basket is closed: all remaining pending
  orders are cancelled and a `CloseBasketCommand` is emitted carrying the
  net open exposure (`closeSide`/`closeVolume`, derived from the running
  buy/sell fill aggregates) that still needs to be flattened at the broker
  (proposal §6). `engine/core` itself never sends a live closing trade —
  it has no I/O — so this is purely data for the driver to act on.
  `LiveRunner::handleCommand` turns it into an actual Market order
  (`OrdType=1`) and does **not** call `restart()` until that order's fill
  is confirmed back (matched by `ClOrdID`, tracked via
  `basketClosePending_`); only then does the next cycle start. Skipping
  this wait was a real bug: `restart()` used to run immediately on
  `EngineStatus::Completed`, so a new range/basket would start seeding
  while the previous cycle's real position was still open at the broker
  — cancelling *pending* orders is not the same as *closing filled ones*,
  and nothing was doing the latter. `BacktestRunner` doesn't need any of
  this: its "position" is just the engine's own P/L aggregates, which
  `resetCycleState()` already clears on the next `start()`/`restart()`,
  so there's nothing external left to flatten.
- Before any new order is armed, the risk engine checks it against
  `RiskLimits`. A breach emits a `RiskAlertCommand` and halts further order
  creation (`EngineStatus::HaltedByRisk`) — proposal §8.

## Driving the engine (backtester or, later, live)

The driver owns everything `StrategyEngine` doesn't:

1. **Trigger detection** — comparing live/replayed price against each
   pending order's price to decide when it "fills". In live trading this is
   the broker's job; in `BacktestRunner` it's done explicitly (a Buy Stop
   triggers when `ask >= price`, a Sell Stop when `bid <= price`).
2. **Executing commands** — actually placing/cancelling orders with a
   broker (live) or updating an in-memory order-book mirror (backtest).
3. **Feeding results back** — turning a real or simulated fill into an
   `OrderConfirmedEvent`/`OrderRejectedEvent` for the engine to react to.

`BacktestRunner` mirrors the engine's pending-order book from the
`PlaceOrderCommand`/`CancelOrderCommand` stream so it always knows what to
check on the next tick, without the engine exposing any internal state.

## Precision model

`Price` and `Volume` (see `types.hpp`) are integer counts of
`SymbolSpec::tickSize`/`volumeStep` units, never raw `double`, in any
decision path. Conversion to/from a human-readable value only happens at
I/O boundaries (JSON config, CSV, log output) — proposal §5.

`LotProgressionConfig::initialLot`/`lotIncrement` are themselves `Volume`
values built against the same `SymbolSpec`, so any linear combination of
them (the progression formula) is automatically an exact multiple of the
broker's volume step — no separate rounding step is needed in
`computeSequenceLot`, only min/max clamping.

## Basket P/L composition

`StrategyEngine::evaluateBasketPnl` computes gross P/L at the theoretical
mid price (`(bid+ask)/2`), then deducts, in line with proposal §6's
formula:

Gross P/L per side is tracked as a running `(totalVolumeSteps,
sumEntryPriceTicksVolume)` pair rather than a list of individual fills
(`pnl.hpp::aggregatePositionPnlMoney`) — a basket can accumulate an
unbounded number of fills over a long trend, and this keeps the state
fixed-size and the per-tick evaluation O(1) regardless.

- **commission** — `totalVolumeLots * CostInputs::commissionPerLotRoundTurn`
- **spread cost** — half the current spread, applied to total open volume;
  this is the cost of actually crossing the spread to exit at market (using
  mid-price gross P/L means the exit cost has to be charged explicitly
  rather than embedded via bid/ask marks, to avoid double-counting)
- **swap** — accumulated via `StrategyEngine` internal state (no external
  rollover-clock dependency yet; a real swap feed is a later-phase concern)
- **slippage** — accumulated at fill time as the adverse difference between
  a pending order's requested price and its actual confirmed fill price

## Known Phase-1 simplifications (intentional, documented rather than silent)

- **Margin usage** is always reported as 0 to the risk engine — real margin
  tracking needs live account equity/leverage data that doesn't exist until
  a broker/account adapter is built (Phase 3+). A `maxMarginUsageFraction`
  risk limit is accepted in config but currently inert.
  Set `maxMarginUsageFraction = 0` in config as your current *documented*
  no-op rather than silently ignoring a limit the user reasonably expects to
  be live.
- **Drawdown** during a single open basket uses current floating loss as a
  proxy, not a true peak-to-trough series — that's tracked properly at the
  backtest level instead (`StatsCollector::onBasketClosed`), across closed
  baskets.
- **Risk breaches never force-close an open basket** — they only stop new
  orders from being armed and raise `RiskAlertCommand`. Whether an
  in-flight basket should be forcibly closed at a loss on a risk breach is a
  product decision left to a later phase/operator judgment, not assumed
  here.
- **Structural order-count invariant**: this strategy shape (two boundaries,
  one pending order per side, replaced not accumulated on fill/reject)
  never has more than 2 pending orders at once. `kMaxTrackedOrders` (8) and
  `kMaxCommandsPerEvent` (9) give headroom over that invariant without
  inflating `CommandBatch` — keep this in mind if a future strategy variant
  needs to hold more simultaneous legs.
