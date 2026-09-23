# cTrader FIX API notes (IC Markets)

`engine/ctrader_fix` is a minimal FIX 4.4 client for **cTrader's** FIX
dialect (IC Markets provides FIX access through cTrader), built against the
official spec: *"cTrader FIX Engine, Rules of Engagement"*, Spotware
Systems Ltd, v2.18.1. This is a condensed reference for the subset this
codebase actually implements — not a replacement for the spec.

## Getting credentials

FIX API credentials are per-account and issued by cTrader itself: in the
cTrader (or cAlgo) desktop/web app, open **Settings → FIX API**. That
screen lists the **Price connection** (Quote session) and the **Trade
connection** separately — each has its own **Host** and **Port**, which
are not assumed to be the same (`LiveConnectionConfig::quote`/`trade` in
`live_config.hpp` map directly to these two blocks). You'll also get a
**BrokerUID**, a numeric **trader login**, and a **password** shared by
both connections. All of this is specific to your account and environment
(demo vs live) and is never guessable or looked up externally. Plain-TCP
FIX ports (this client does not yet support TLS — see "Known limitations"
below) are commonly in the 5201/5202 range, but treat whatever your
account's settings page shows as authoritative.

The same settings screen typically also lists a **Symbol Id** table
(instrument name → numeric id) for reference — you do **not** need to copy
that into the config; `LiveRunner` resolves the configured `symbolName`
(e.g. "XAUUSD") to its numeric id automatically at startup via
`SecurityListRequest` (see "Symbol resolution" below).

## Session model

Two independent FIX sessions per account, differing in both endpoint and
`TargetSubID (57)`:

- **Quote** ("Price connection") session: `TargetSubID=QUOTE`,
  `SenderSubID=Quote`. Used for `MarketDataRequest`/
  `MarketDataSnapshotFullRefresh`/`MarketDataIncrementalRefresh`.
- **Trade** session: `TargetSubID=TRADE`, `SenderSubID=Trade`. Used for
  everything order-related.

`SenderCompID (49)` = `"<environment>.<BrokerUID>.<TraderLogin>"` (e.g.
`demo.icmarkets.12345`); `TargetCompID (56)` is always `"CSERVER"`. This
identity, plus Username/Password, is the same on both connections — only
the endpoint and TargetSubID/SenderSubID differ.

## Orders

There is no separate "Buy Stop"/"Sell Stop" order type. Every pending order
this engine places is `OrdType(40)=3` (Stop) with `StopPx(99)` set to the
boundary price; direction comes from `Side(54)` (1=Buy, 2=Sell) — see
`muuk::ctrader_fix::buildNewOrderSingle`. `ClOrdID(11)` is always the
engine's own numeric `OrderId` (from `muuk::core::PendingOrder`), stringified
— this avoids needing a separate id-translation table: an incoming
`ExecutionReport`'s `ClOrdID` parses straight back into the `OrderId` to
feed into `StrategyEngine::onOrderConfirmed`/`onOrderRejected`.

`ExecType(150)=F` ("Trade") is the actual fill — *not* the initial
acceptance of a pending Stop order, which arrives first as `ExecType=0`
("New") with no engine action needed (`engine/core` already updated its
own state the moment it emitted the `PlaceOrderCommand`).

**`OrderQty(38)` is in lot hundredths, not raw lots** — confirmed
empirically against a real IC Markets demo account: sending `OrderQty=2`/
`3` opened `0.02`/`0.03`-lot positions, and a `TRADING_BAD_VOLUME` reject
citing a "1.00" minimum turned out to mean 0.01 lot, not 1.00 lot. This
matches cTrader's platform-wide volume convention (hundredths of the
displayed unit). `engine/core`'s `SymbolSpec`/`Volume` stay in real lots
throughout — for P&L math, risk checks, display, everything — so the ×100
conversion is applied only at the wire-encoding boundary in
`LiveRunner::handleCommand` (`engine/ctrader_fix/src/live_runner.cpp`),
not baked into `engine/core`.

## Symbol resolution

`Symbol (55)` is a **broker-assigned numeric id**, not a string like
"XAUUSD". The request form of `SecurityListRequest` where `Symbol(55)` is
*omitted* returns the full instrument list (with `SymbolName(1007)` per
entry); `LiveRunner::resolveSymbol` sends that unfiltered request and
matches the configured symbol name client-side — see
`muuk::ctrader_fix::buildSecurityListRequest`/`parseSecurityList`.

## Auto-centered range (`connection.autoRangeWidthPoints`)

Manually re-editing `strategy.range` before every run to match wherever
the market currently is turned out to be real, recurring friction (the
market moves; a range set an hour ago can be tens of dollars away by the
time you next start the bot, causing every pending order to trigger
immediately and cascade). Setting `connection.autoRangeWidthPoints` to a
non-zero total width (e.g. `0.20`) makes `LiveRunner` ignore
`strategy.range`'s configured lower/upper entirely and instead center a
range of that width around the midpoint of the *first live quote* it
receives after connecting — see
`LiveRunner::waitForFirstQuoteAndCenterRange` in `live_runner.cpp`. This
is why quote subscription now happens *before* the engine (and so the
initial orders) is constructed: a real tick has to arrive first. Leave it
at `0` (default) to use `strategy.range` exactly as written — this is
what the backtester and CLI examples still do, since there's no "live
price" concept there.

This centers the range once, at startup. It is *not* consolidation/
support-resistance detection (not built) — it does not pick where a range
"should" be, only where the market already is right now.

## Continuous operation across cycles

Two things combine to keep the strategy following live price for the
whole time it runs, not just at startup:

- `StrategyConfig::seedOffsetPoints` (`engine/core`, see
  `docs/architecture.md` "Seed anchor mode") re-derives the range from a
  real confirmed fill every time `armInitialOrders()` runs — including on
  `restart()` after a basket closes, using the tick that just triggered
  that close as the reference, not the (by then closed and possibly
  stale) previous cycle's range.
- With `connection.autoContinue` on (the default), `LiveRunner` starts the
  next cycle itself once a basket has closed -- but only after every one of
  its positions is confirmed closed at the broker, no pending order is left,
  and `connection.basketCooldownSeconds` has passed. It never opens a range
  on top of an unfinished one. With `autoContinue` off, ranges only open
  when the operator calls `openRange()` (the desktop app's **Open range**
  button), under the same "account must be flat" rule.

Together, `autoRangeWidthPoints` (range width, re-centered on live price
each time a range opens) + `seedOffsetPoints` (StrategyEngine, every
cycle) is the combination meant for unattended, continuous operation.

**Cancel-vs-fill race.** An order `LiveRunner` has asked the broker to
cancel can still fill before the cancel lands (most likely for a seed
order right when its sibling fills). Every sent cancel is remembered
(`cancelledOrderInfo_`) until the broker confirms it; if a fill arrives
for one instead, that surplus position is closed immediately with its own
Market order rather than left open for the rest of the cycle. If the
broker rejects the cancel, the order goes back on the pending list.

## Operator controls (desktop app / `LiveRunner`)

- `openRange()` / `closeRange()` -- open a new range centered on live price;
  or cancel all pending orders, close all positions and go Idle.
- `closeAllPositions()` / `cancelAllPendingOrders()` -- either half of
  `closeRange()` on its own (also leaves the engine Idle).
- `closePosition(id)` / `cancelOrder(id)` -- one position or order, leaving
  the rest of the running cycle alone.
- `applySettings()` -- range width, lots, profit target, seed offset,
  cooldown, risk limits, auto-continue; cycle-shaping values take effect
  from the next range. `saveLiveSettings()` writes them back into the
  config file without touching the credentials.

## Who detects a pending order's trigger?

The broker does — that's the point of placing a real Stop order. Unlike
`BacktestRunner`, which must simulate trigger detection itself (comparing
each tick against pending order prices, since there's no real broker),
`LiveRunner` only ever *reacts* to `ExecutionReport`s; it never inspects
price against a pending order's `StopPx` itself.

## Known limitations (intentional, see `fix_session.hpp`'s class comment)

- **No TLS.** Only plain TCP is implemented, to avoid adding an OpenSSL
  dependency that would break the "no manual installs, everything via
  `FetchContent`" build story. Fine for demo trading over `FetchContent`
  -installed dependencies; revisit before any live/production use.
- **No FIX message-store replay on `ResendRequest`.** Every session logs
  on with `ResetSeqNumFlag=Y`, so this only matters within one connection's
  lifetime — a dropped connection is surfaced via the disconnect handler
  for the caller to react to, not silently recovered.
- **No full order-book depth handling.** Market data is requested as
  `MarketDepth=1` ("spot"/top-of-book) — `MDUpdateAction` DELETE semantics
  (relevant only for full-depth subscriptions) are not interpreted.
- **A risk-limit breach halts new order creation but never force-closes an
  open basket** — same documented choice as `engine/core` (see
  `docs/architecture.md`).
