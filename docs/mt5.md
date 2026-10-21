# MetaTrader 5 (cent accounts) -- run from the Muuk app

Muuk trades cTrader over FIX. Cent accounts (JustMarkets Standard Cent,
RoboForex ProCent, ...) are on MetaTrader 5, which has no FIX or trading API
for outside programs: only code running inside MT5 may trade there. So Muuk
brings its own piece for the inside, **and puts it there itself** -- you
control everything from the Muuk app, exactly as with cTrader:

```
 Muuk app (GUI) / muuk-live
   ├─ starts MetaTrader 5 (minimized), logs in, turns algo trading on,
   │  and attaches MuukBridge (built into Muuk, compiled by MT5's MetaEditor)
   ├─ MuukBridge  <── named pipe \\.\pipe\MuukBridge_5200 ──>  bridge
   └─ LiveRunner  <── the same FIX as cTrader, 127.0.0.1:5201 ──> bridge
```

Everything works the same (range, lots, auto target, margin guards, lock,
escape, audit, history, reports). On top, MT5 gives what FIX never did:
the **exact balance, equity, margin, free margin and swap**, the account
currency (USC = cent) and the symbol's lot sizes.

## Use it

1. Install your broker's MetaTrader 5 and open it **once** (so Windows has
   its data folder). Close it again -- Muuk starts it from now on.
2. Muuk: **Settings > Account > Platform: MetaTrader 5**:
   - MetaTrader 5: which MT5 (each broker installs its own; "auto" = the first);
   - Login, Password, Server: the MT5 account (empty login = the one MT5 used last);
   - Account type: **Demo** or LIVE -- must match the MT5 account: a REAL MT5
     account under a Demo config is refused before any order;
   - Save.
3. **Connect.** Muuk starts MT5 minimized with the bridge on a chart; the header
   shows `MT5 <login>` (green, "cent" on a cent account) and the bot trades.

MT5 keeps that chart, so the next time MT5 opens (by Muuk or by you) the
bridge runs by itself. With MT5 already open and no bridge on a chart, close
MT5 and press Connect (or "Start MT5" in the header).

### The MT5 account

**The bot trades only the config's account.** Settings > Account > Login names it. If MT5 is logged in to any other account, the bot refuses to connect, so a live account under a demo config, or someone else's account, is never traded.

**Muuk switches MT5 by itself.** When MT5 opens on another account:
- Muuk closes MT5 (as its X button does) and opens it again, logged in to the config's account;
- it tries this twice;
- then the **MT5 accounts** list opens.

The switch can be turned off in that list.

**The MT5 accounts list** (the *Accounts* button next to the MT5 pill in the header):
- Every account MT5 has been logged in to on this PC is remembered with its server, company and demo/real. Passwords are never remembered.
- Click one to trade it:
  - the config's login and server are set to it;
  - the Account type follows its demo/real;
  - MT5 is switched to it.
- *Add or fix an account* takes a login, password and server for one MT5 can't log in to by itself. The password goes into this config file only.

**Logging in:**
- A config that gives only the login still logs in, on the server that account was last seen on.
- The config's saved password belongs to its own login: changing the login never sends one account's password to another.
- An account without a password in the config logs in with the one MT5 saved for it.

While the bot is connecting, Muuk heals this by itself: when the bridge hasn't
connected for a minute after MT5 was started, Muuk closes MT5 (as its X button
does) and starts it again with the bridge -- up to 3 times, counted again once
the bridge connects.

The bridge's chart is the account's **own** gold symbol (`XAUUSD.c` on a
JustMarkets cent account, `XAUUSDm` on Exness...): the one the bridge traded on
that login before (`MQL5\Files\MuukBridge_<login>.symbol`), else the gold
symbol the broker server sent prices for (`bases\<server>\ticks`), else a gold
chart MT5 already has. A chart of a gold symbol the account can't trade (a
plain `XAUUSD` on a cent account) never loads -- MT5's journal says
"initializing of MuukBridge failed ... (symbol synchronization timeout /
execution canceled)" -- and the bridge never starts. `chartSymbol` in the
config can still name one by hand.

The account must be **hedging** (a netting account would close a position
instead of locking it) -- the bridge refuses netting accounts.

## What the bridge does inside MT5

- Only the account's gold symbol (XAUUSD, XAUUSDc, GOLD, ... found by
  itself) and only orders/positions with its magic number 770077: manual
  trades and other EAs are never touched.
- Pending Buy/Sell Stops for the range, market orders for locks and closes
  (a close is aimed at its own position ticket), cancels.
- Every fill, prices (polled every 25 ms), the account every second, the
  per-position swap. The bot's order id is kept in each order's comment, so
  a restarted MT5 still knows its orders.

## Any broker: taken from MT5 on connect

- price digits / step (2 or 3 digits), lot min / step / max, the broker's
  pending-order limit (max orders kept 2 below it);
- a first lot below the broker's minimum is raised to it (RoboForex ProCent
  gold: 0.10);
- money: P/L, commission and margin are counted in the account's currency --
  **US cents on a cent account** (shown as `12.50¢`), whatever the broker's
  lot convention:

| Broker | 1 lot | $1 move on 0.01 lot | Muuk counts |
|---|---|---|---|
| JustMarkets Standard Cent | a cent lot (1/100 of standard) | 1 USC | 100 per 1.00 per lot |
| RoboForex ProCent | 100 oz, min 0.10 | 100 USC | 10,000 per 1.00 per lot |

- margin: MT5's own margin for 1 lot at the current price and the symbol's
  hedged-margin rate (the Settings balance / leverage are only for cTrader).

Cent accounts are usually commission-free (`commissionPerMillionPerSide` 0)
with a wider spread -- the auto target follows the real spread.

## Speed

Stop orders sit at the broker, so their speed is the broker's. The bridge
adds up to one 25 ms poll for fills and market orders -- small next to a $2
range. Keep MT5 and Muuk on the same PC/VPS.

## Tested

The bridge and the whole bot are tested with a simulated MT5 over the same
pipe (`engine/mt5_bridge/tests`); the bridge compiles cleanly in MetaEditor.
First run on a **demo** account: open a range, let a level fill, press
Close range, and check that MT5's positions and history match Muuk's.
