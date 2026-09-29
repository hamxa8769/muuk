# MetaTrader 5 (cent accounts) through the Muuk bridge

Muuk trades cTrader over FIX. Cent accounts (JustMarkets Standard Cent,
RoboForex ProCent, ...) are on MetaTrader 5, which has no FIX -- so Muuk
brings a bridge:

```
 MetaTrader 5 ── MuukBridge.mq5 (EA on the gold chart)
                     │  JSON lines, 127.0.0.1:5200
                 Muuk app / muuk-live  ── bridge (mt5_gateway)
                     │  the same FIX as cTrader, 127.0.0.1:5201
                 LiveRunner: engine, guards, audit, history, app
```

Everything in Muuk works the same (range, lots, auto target, margin guards,
lock, escape, audit, history, reports). On top, MT5 gives what FIX never
did: the **exact balance, equity, margin, free margin and swap**, the
account currency (USC = cent) and the symbol's lot sizes.

## Set up (once per MT5 terminal)

1. MT5: **File > Open Data Folder** > `MQL5\Experts`, copy `MuukBridge.ex5`
   there (from the package's `mt5` folder). With only `MuukBridge.mq5`:
   copy that, open it in MetaEditor (F4) and press **Compile**.
2. MT5: **Tools > Options > Expert Advisors**:
   - tick **Allow algorithmic trading**;
   - tick **Allow WebRequest for listed URL** and add `127.0.0.1`
     (MT5 uses that list for sockets too).
3. Open the **gold chart** of that account (XAUUSD, XAUUSDc, XAUUSD.m -- any
   name) and drag **MuukBridge** onto it. Inputs: `BridgePort` 5200 (same as
   the config), `MagicNumber` 770077. The Experts tab says
   `MuukBridge: connected to Muuk` once the app runs.
4. Muuk: **Settings > Account > Platform: MetaTrader 5 (Muuk bridge)**, or
   start from `mt5_account.example.json`. No password -- MT5 is already logged in.
5. Connect. The header shows `MT5 <login>` (green) and "cent" on a cent account.

The account must be **hedging** (a netting account would close a position
instead of locking it) -- the bridge refuses netting accounts.

## What the EA does

- Only this chart's symbol, and only orders/positions with its magic number:
  manual trades and other EAs are never touched.
- Pending Buy/Sell Stops for the range, market orders for locks and closes
  (a close is aimed at its own position ticket), cancels.
- Every fill (from `OnTradeTransaction`), prices on every tick (and every
  25 ms), the account every second, the per-position swap.
- The bot's order id is kept in each order's comment, so a restarted EA still
  knows its orders.

## Cent accounts and money units

The EA reports the symbol's tick value in the account's currency, and Muuk
counts P/L, commission and margin in that money -- **US cents on a cent
account** (shown as `12.50¢`). Brokers do lots differently:

| Broker | 1 lot | $1 move on 0.01 lot | Muuk counts |
|---|---|---|---|
| JustMarkets Standard Cent | a cent lot (1/100 of standard) | 1 USC | 100 per 1.00 per lot |
| RoboForex ProCent | 100 oz, min 0.10 | 100 USC | 10,000 per 1.00 per lot |

Either way the numbers match what MT5 shows. Set the lots for the broker's
minimum (RoboForex ProCent gold: First lot 0.10); the audit warns if the first
lot is below it. Set the costs: cent accounts are usually commission-free
(`commissionPerMillionPerSide` 0) with a wider spread -- the auto target
follows the real spread.

Margin: the guards use MT5's own margin (the EA sends the margin for 1 lot at
the current price) and the symbol's hedged-margin rate, not the Settings
balance/leverage.

## Speed

Stop orders sit at the broker, so their speed is the broker's. The bridge
adds one local hop (EA timer 25 ms at most + a few ms) for fills and market
orders -- small next to a $2 range. Keep MT5 and Muuk on the same PC/VPS.

## Not tested against a real MT5 yet

The bridge is tested with a simulated EA (`engine/mt5_bridge/tests`), and the
EA compiles cleanly in MetaEditor. Try it on a **demo** cent account first:
open a range, let a level fill, press Close range, and check that MT5's
positions and history match Muuk's.
