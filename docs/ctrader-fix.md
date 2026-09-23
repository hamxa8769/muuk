# cTrader FIX API notes (IC Markets)

`engine/ctrader_fix` is a minimal FIX 4.4 client for **cTrader's** FIX
dialect (IC Markets provides FIX access through cTrader), built against the
official spec: *"cTrader FIX Engine, Rules of Engagement"*, Spotware
Systems Ltd, v2.18.1. This is a condensed reference for the subset this
codebase actually implements — not a replacement for the spec.

## Getting credentials

FIX API credentials are per-account and issued by cTrader itself: in the
cTrader (or cAlgo) desktop/web app, open **Settings → FIX API**. You'll get
a **BrokerUID**, a numeric **trader login**, a **password**, and a
**host/port** for both a Quote and a Trade connection — these are specific
to your account and environment (demo vs live) and are never guessable or
looked up externally. Plain-TCP FIX ports (this client does not yet
support TLS — see "Known limitations" below) are commonly in the 5201/5202
range, but treat whatever your account's settings page shows as
authoritative.

## Session model

Two independent FIX sessions per account, same TCP host/port, same
identity, differing only in `TargetSubID (57)`:

- **Quote** session: `TargetSubID=QUOTE`, `SenderSubID=Quote`. Used for
  `MarketDataRequest`/`MarketDataSnapshotFullRefresh`/
  `MarketDataIncrementalRefresh`.
- **Trade** session: `TargetSubID=TRADE`, `SenderSubID=Trade`. Used for
  everything order-related.

`SenderCompID (49)` = `"<environment>.<BrokerUID>.<TraderLogin>"` (e.g.
`demo.icmarkets.12345`); `TargetCompID (56)` is always `"CSERVER"`.

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

## Symbol resolution

`Symbol (55)` is a **broker-assigned numeric id**, not a string like
"XAUUSD". The request form of `SecurityListRequest` where `Symbol(55)` is
*omitted* returns the full instrument list (with `SymbolName(1007)` per
entry); `LiveRunner::resolveSymbol` sends that unfiltered request and
matches the configured symbol name client-side — see
`muuk::ctrader_fix::buildSecurityListRequest`/`parseSecurityList`.

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
