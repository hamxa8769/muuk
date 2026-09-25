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

**Closing a position on a hedging account needs `PosMaintRptID(721)`.**
From the spec (NewOrderSingle): *"A position ID where this order should be
placed. If not set, a new position will be created and its ID will be
returned in the Execution Report message. It can be specified only for
hedged accounts."* So an opposite-side Market order **without** 721 opens
a second position instead of closing the first -- which is exactly what
the bot's close buttons did until this was fixed. `LiveRunner` now keeps
each fill's 721 (the ExecutionReport carries it) and sends it on every
closing order (`buildMarketOrderSingle(..., positionId)`); a position whose
id is unknown is never "closed" blindly. `TimeInForce(59)` is documented as
*"Deprecated, this value will be ignored"* -- Market orders are always IOC.

**Position sync.** `Request For Positions (35=AN)` without 721 returns one
`Position Report (35=AP)` per open position (`PosMaintRptID`, `Symbol`,
`LongQty`/`ShortQty` in the same units as `OrderQty`, `SettlPrice` = average
price), or a single report with `PosReqResult(728)=2` if there are none.
`LiveRunner` does this on connect (`connection.syncPositionsOnConnect`,
default on) and on demand (`syncPositions()`, the app's **Sync from
broker**), adopting positions it didn't open this session so they can be
closed from the app, and never opening a range while any are open.
Confirmed against the IC Markets demo server.

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
  or cancel all pending orders, close all positions and go Idle. After
  `closeRange()`, with auto-continue on, a new range opens around live price
  `connection.manualCloseCooldownSeconds` (default 15; Settings → "Cooldown
  after Close range") after the account is confirmed flat. The countdown
  shows as "Cooldown" in the app, and 0 means wait for Open range instead.
  The exit guard uses `closeRange(false)`, which never reopens.
- Dropped connections are retried after 5, 10, 20, 40, then every 60 s, by
  both `muuk-desktop` ("Reconnect by itself", Performance tab) and
  `muuk-live` (`--no-reconnect` turns it off).
- `closeAllPositions()` / `cancelAllPendingOrders()` -- either half of
  `closeRange()` on its own (also leaves the engine Idle).
- `closePosition(id)` / `cancelOrder(id)` -- one position or order, leaving
  the rest of the running cycle alone.
- `applySettings()` -- range width, lots, profit target, seed offset,
  cooldown, risk limits, auto-continue; cycle-shaping values take effect
  from the next range. `saveLiveSettings()` writes them back into the
  config file without touching the credentials.
- `flattenAndWait(timeout)` -- the exit guard: does what `closeRange()`
  does, then blocks until the broker has confirmed every close (returns
  true), or a close is rejected / the connection drops / the timeout
  passes (returns false and logs a warning). Used by:
  - `muuk-live` on Ctrl+C, Ctrl+Break, closing the console window, logoff
    and shutdown (skip with `--keep-open-on-exit`);
  - `muuk-desktop` on Windows logoff/shutdown/restart (it holds the
    shutdown with "Muuk is closing open positions" for up to 8 s). The
    window's close button instead asks: *Close everything & exit*, *Exit,
    keep them open*, or *Cancel*.

  Nothing on the client side can protect against a crash, a killed
  process, or the machine losing power or network -- open positions then
  stay at the broker unmanaged. See `docs/vps.md`.

## Basket history, reports and logs

Every basket is written, the moment it closes, as one JSON line to
`muuk_history/<config name>.baskets.jsonl` next to the config file, one
file per account file (an older shared `baskets.jsonl` moves to the first
account that opens it): start/close time,
range, close reason, and each position (side, lots, Stop price, real entry
and exit fills, gross, commission, net, broker position id). The desktop
app's **History** tab shows them as a tree (basket -> positions), and
**Export PDF report** writes `muuk_reports/muuk_report_<time>.pdf`:
overview (all-time and this session), system speed (quote -> decision and
fill -> next order in microseconds, broker round trip in milliseconds;
live/avg/p50/p99/max), settings, per-basket and cumulative P/L charts, a
table of every basket, and every basket's position tree. **Save logs**
writes `muuk_logs/muuk_<time>.log`; the log is also saved there
automatically on every disconnect.

The desktop app also has:
- **Volume**: lots traded per day, week or month (and today / this week /
  this month / all time at a glance): baskets won and lost, lots, broker
  volume (both sides, i.e. twice the lots), value traded (lots x contract
  size x entry price), lots in winning vs losing baskets, commission, net,
  and net per lot. The same tables are in the PDF report.
- **Reset data**: a fresh start. The history and the running log move to
  `archive/` folders (nothing is deleted), and the statistics start over.

**Commission is worked out exactly like cTrader does**, because FIX can't
report it: neither the Execution Report nor the Position Report carries
commission, swap or balance (see the cTrader FIX specification). cTrader
brokers charge per value traded, not per lot:
`costs.commissionPerMillionPerSide` (Settings > "Commission ($ per $1M)";
IC Markets Raw = 30, i.e. $3 per $100k). For each position:

    one side  = lots x contract size x entry price x rate / 1,000,000, rounded to the cent
    commission = 2 x one side   (open + close -- cTrader's "Commission" column)

0.32 lot of XAUUSD filled at 4270.80: 0.32 x 100 x 4270.80 x 30 / 1M =
4.0999 -> $4.10 a side, $8.20 round trip -- the figure cTrader shows. A
flat `$ per lot` rate (`commissionPerLotRoundTurn`) is only used when no
per-million rate is set. At gold ~4270 the real cost is ~$25.6 per lot, so
the old flat $7 made every basket look ~$18.6 per lot traded better than
the broker's balance. The profit target and stop loss count exactly this
commission, so a $20 target is $20 after commission.

The Positions tab uses cTrader's own columns: **Gross** = actual fill ->
the price it would close at now (bid for a buy, ask for a sell; spread and
slippage are inside it), **Commission**, **Net**. Each basket in History
shows the net when the target fired and the net the closing fills really
booked -- the booked figure is what the broker credits. Swap (only for a
basket held past the daily rollover) is not reported over FIX and is not
included.

## Broker check: the app always matches the broker

Every `connection.verifySeconds` (default 30 s), and on every connect, the
bot asks the broker for all open positions (Request For Positions) and all
working orders (Order Mass Status Request), and compares them with its own
view:
- **A pending order the broker doesn't have** is placed again (`MISSED`).
- **A position of the running basket that the broker no longer has**
  (closed in cTrader) stops the basket: its pending orders are cancelled,
  the rest is listed, and nothing new opens by itself. Its P/L and next
  order can't be trusted after that.
- **An order or position the bot didn't make** is listed as "old" and
  reported. This usually means Muuk is also running on another PC/VPS with
  the same account.
- Orders the broker hasn't acknowledged within 8 s are checked the same
  way.

In the app:
- The header shows "Broker verified HH:MM:SS" (green), or "Broker check:
  issues" (red, with the details).
- Positions show the broker's own position id, green once confirmed (it's
  the same id cTrader shows).
- Pending orders show *confirmed* / *sent...* / *not sent*.
- The header also shows which broker and account the bot is logged into,
  and DEMO / LIVE MONEY.

**ClOrdIDs start from a time-based number on every connect**
(e.g. 1790270000001). Another copy of the bot on the same account never
uses the same ids, so the broker's reports for its orders are never
mistaken for this one's.

**Every basket is measured:**
- how it ended: target / stop loss / manual / interrupted / failed;
- orders placed, filled, rejected, and missed and re-placed;
- its deepest drawdown and best point;
- the most lots open at once.

The History tab sums these up: outcomes, success rate, deepest drawdown.

## Lot sizes

`strategy.lotProgression`:
- `initialLot` + `lotIncrement`: 0.01, 0.02, 0.03... (the default).
- `lotMultiplier` > 1 instead multiplies: `2` gives 0.01, 0.02, 0.04,
  0.08, 0.16... (rounded to the 0.01 step).
- Settings → *Lot mode* switches between the two and previews the
  sequence, the total lots, and the order at which *Max lot* / *Max
  orders* stop the basket from adding more.
- Doubling recovers after a short move once a few orders have filled, but
  every back-and-forth doubles the loss. With a $2 range: about −$170 at
  the 8th fill and −$680 at the 10th (before costs). Always pair it with a
  **Stop loss**.

## Accounts

Settings → *Account* edits the broker login of the current config file:
account type (demo/live), account number, password, broker UID,
TargetCompID, price/trade host and port, and symbol.
- Pasting the SenderCompID from cTrader (`demo.icmarkets.12345678`) fills
  type, broker and account number.
- The password is never shown. Leaving it empty keeps the saved one.
- *Save as new account* creates a new config file with the current strategy
  settings and switches to it. Each account file has its own state and
  history.
- The header shows **DEMO** or **LIVE MONEY** for the file in use.

## Persistence: the bot picks up where it left off

Nothing is lost when the app closes, the connection drops or the PC
restarts. Everything lives next to the config file:

| File | What's in it |
|---|---|
| `muuk_state/<config>.state.json` | The running basket: engine state, open positions, pending orders, closes in flight. Also id counters, so no ClOrdID is ever reused, and lifetime statistics (speed totals, basket numbering). Written on every change, atomically. |
| `muuk_history/<config>.baskets.jsonl` | Every closed basket of that account. |
| `muuk_logs/muuk_live.log` | The log, line by line. The app shows its tail again on the next start. |
| the config file itself | Settings. **Save & apply** writes them there, so they're what the app opens with. |
| `%APPDATA%\Muuk\desktop.json` | App preferences: last config, theme, *Connect when the app opens*. |

On connect, `LiveRunner` asks the broker for every open position (Request
For Positions) and every working order (Order Mass Status Request, `35=AF`,
`585=7`). It then compares them with the saved basket:

- **Everything still there**: the basket carries on with the same lot
  sequence, target and range. Nothing new is sent.
- **A pending order filled while the bot was away**: the order is gone,
  and a position with the same side and size opened near its Stop price.
  It is processed as the fill it was, so the next order goes out at once.
- **A close was in flight**: if the position is still open, the close is
  sent again. If it's gone, the leg is recorded as closed while
  disconnected (the exit price isn't reported, so its P/L can't be).
- **Something changed that the bot can't explain** (a position closed or
  an order cancelled in cTrader, or the broker didn't answer): the basket
  is **not** resumed. Its pending orders are cancelled, its remaining
  positions are listed, and no range opens by itself. The app says why.

Positions and working orders on the symbol that aren't part of the saved
basket are listed as "old", so they can be closed or cancelled from the
app. Orders whose ClOrdID isn't a bot order id (placed by hand) are left
alone.

**A basket found at the broker with no saved state** (left by an older
version, or the state file was lost) is picked up if it looks exactly
like one of the bot's baskets. That means lots in the configured sequence
(0.01, 0.02, 0.03...), sides alternating, and the first on the configured
start side. The range is rebuilt from the first fill, and the next order
is placed.

**Every running basket always has its next order.** Three things make sure
of it, and each one logs `MISSED`:
- A pending order the broker cancels or expires, or that is cancelled by
  hand in cTrader, is placed again. Cancelling one from the app is left
  alone.
- A watchdog, about once a second: if an order the engine counts on isn't
  at the broker, it is placed again. If the basket has positions but no
  pending order at all, its next order is placed (at most 3 tries per
  fill, so a rejecting broker isn't flooded).
- On reconnect: a saved pending order that is gone without a fill is
  placed again.

**Orders the market has already passed wait.** A Buy Stop at or below the
ask, or a Sell Stop at or above the bid, would be refused by the broker.
Such an order is held ("waiting" in Pending orders) and sent as soon as
the price is back on the right side. **Place now** sends it at market
instead; its fill then carries the basket on like any other.

**Slippage** (fill vs the order's own price) is logged on every fill
(`[fill] ... slippage`). It is shown per position (Positions tab), per
leg (History tab), per period (Volume tab) and in the PDF position trees.

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

## Auto profit target

`strategy.autoProfitTarget: {"enabled": true, "percent": 240}` (the gear next
to the net P/L bar): instead of a fixed dollar target, the basket is booked
once its net profit reaches *percent* of what the open positions cost --
commission (open + close) plus the full spread. One 0.01 lot aims low; every
fill raises the target. In the app it's a level: 20 / 40 / 60 / 80 / 100 =
200 / 240 / 280 / 320 / 360 % of the cost (Settings > Auto profit target, or
the gear). A config with less than 200 % is read as 200 %. Target, stop
loss and the auto target apply at once, also to a running basket.

## Market hours, sessions and the news guard

`"market": {...}` in the config (Settings > Market hours & news). Only
*new* ranges wait; a running range keeps being managed (target and stop
loss), unless "When a pause starts" says to close it (before the weekend,
or at every pause).

- **Sessions** on their own city's clock, daylight saving included:
  Sydney 07-16, Tokyo 09-18, London 08-17, New York 08-17, Mon-Fri.
  "Trade in" limits new ranges to the ticked sessions.
- **Session-end pause**: from 15 min before to 1 min after each ticked
  session ends (and the first minute after the daily reopen).
- **Gold's hours**: Sunday 18:00 to Friday 17:00 New York time, daily
  break 17:00-18:00 -- shown as "Weekend" / "Daily break".
- **News guard** (on by default): ForexFactory's weekly calendar
  (`nfs.faireconomy.media/ff_calendar_thisweek.json`, the export of
  forexfactory.com/calendar) is downloaded at most once an hour and cached
  (`%APPDATA%\Muuk\news_calendar.json`; `muuk_state/news_calendar.json` for
  muuk-live). No new range from 15 min before to 15 min after each red
  (High) -- optionally also orange (Medium) -- release in the listed
  currencies (USD). Releases can also be added by hand in the News tab.
- Every basket records the session(s) open when it started; History and
  the PDF report show the results per session.

## Audit and risk

The bot checks itself about once a second (Audit tab; the header pill turns
amber / red when something is off) and fixes what it can by itself:

| Check | What it does |
|---|---|
| Prices from the broker | No price for `quoteStaleSeconds` (120) while the market is open: the feed is dead -- the bot reconnects (at most every 10 min) and resumes the saved basket. |
| Positions match the broker | Every `verifySeconds` (30) the broker's position and order lists are compared with the bot's: missing pending orders are placed again, a basket position closed outside the bot stops the range safely, unknown positions are listed. |
| Pending order in place | A running range always has its next order; placed again if missing (3 tries). |
| Closing orders | A close not confirmed within `closeConfirmSeconds` (10), rejected, or answered "Unknown position" is checked against the broker: already closed there -> booked as closed; still open -> closed again (3 tries), then it waits for the operator. Before, a rejected or unanswered close left the basket stuck. |
| Profit target / stop loss | Net past the target or stop loss but the basket still open -> closed. |
| Bot vs its positions | Warns when the range's own lot count differs from its positions (hand close, partial fill). |
| Daily loss limit | `connection.dailyLossLimitMoney`: today's booked + floating net at minus this closes everything; no new range until the next local day. |
| Risk settings | Warns while there is no stop loss (especially with multiplying lots) or no daily loss limit. |

What it found and did is listed in the Audit tab and written to the log as
`[audit]` lines.

## Small accounts: risk calculator, break-even escape, lock & pause

- **Risk calculator** (Settings > Risk & capital): for the current settings,
  level by level, the lot, total lots, worst floating loss (price bouncing
  between the lines, with commission and spread), margin (larger side at
  `connection.leverage`), equity left from `connection.accountBalance`, and
  the move needed to recover. Rows turn amber at margin call (<100 %) and red
  at stop-out (<50 %). $1000 at 1:500 with $2 range and x2 lasts 7 levels.
- **Lot presets**: Aggressive x2, Balanced x1.5, Safe +0.01.
- **Break-even escape** (`strategy.breakEvenEscape: {"fromLevel": 5,
  "profit": 0.5}`): from that fill on, the target drops to $0.50 net -- a
  deep basket closes at the first bounce back.
- **Lock & pause** (`connection.lockAtLossMoney`): at that floating loss the
  pending orders are cancelled and one opposite market order for the net
  lots freezes the loss; no new range until the operator closes it (Close
  range). A lock does **not** turn the loss into profit: P/L(p) = sum q(p-e)
  becomes constant, and hedging at any moment has the same expected result
  as the loss at that moment (price has no memory) -- plus spread,
  commission and nightly swap. It only stops the loss from growing.
