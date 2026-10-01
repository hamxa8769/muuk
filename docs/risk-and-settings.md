# Risk & capital settings, and what changed

Last updated: 30 September 2026.

This note covers two things:
- what changed in Muuk recently;
- what every setting on the **Risk & capital** page does, with examples.

**Money units.** Every amount is in the account's own money:
- **dollars** on a standard account;
- **US cents (¢, USC)** on a cent account, where 100¢ = $1.

On a cent account, 0.01 lot earns or loses **1¢ per $1 move** of gold. On a standard account the same 0.01 lot earns or loses **$1**. So all the examples below read the same on both accounts: just change "¢" to "$".

---

## 1. Recent changes

### MetaTrader 5 (for JustMarkets, RoboForex and other MT5 brokers)

- **Muuk controls MT5 itself.** Pick *MetaTrader 5* in Settings → Account. Muuk then:
  - opens MT5;
  - uses the account that is already logged in there;
  - installs and starts its bridge on a gold chart.

  You don't attach an EA by hand and you don't type any details (`10d2dbe`, `4254671`).
- **The broker's symbol is used as it is.** This includes names like `XAUUSD.c`, the digits, the lot min/step/max and the order limit (`81066b7`, `c44bea9`).
- **Cent vs standard is detected** from the account currency (USC = cent).
- **Balance, equity, margin, leverage, hedged margin and swap come straight from MT5.**
- **Safety checks:**
  - a REAL account under a demo config is refused before any order;
  - a netting account is refused (the strategy needs hedging).
- **Commission comes from the broker** (`9664ff3`):
  - the rate is learned from the account's gold deals of the last 90 days;
  - every fill reports the commission + fee it really cost, and the basket books exactly that.

  JustMarkets Standard Cent charges **0 commission**. Its cost is the spread.

### Protection against a margin call

These came in `0884b78`:

- **Margin guards:**
  - hold the next level under 300% margin level (a lock stop waits there instead);
  - lock at once under 200%.
- **Break-even close while guarded or locked.**
- **Escape from a lock** (off by default), with a trailing re-lock and a cap on tries.
- **Reverse-trade calculator:** the lot needed to lock a basket, or to win the loss back after a move, with the margin it needs.
- **Order retries:** market orders for a lock or an escape are retried 5 times, half a second apart.

### Costs and slippage

- **Slippage counts the closing fills too** (`ca77cbd`).
  - *Entry slippage:* each Stop fill vs its order price.
  - *Exit slippage:* what the closing fills booked vs the net when the target fired.

  The Volume page shows both.
- **On the JustMarkets demo, all 38 Stop fills of the first run filled exactly at their order price.** MT5's own journal confirms it. Entry slippage 0 is real, not a bug.
- **Exit slippage over the first 24 baskets: +0.60¢ in your favour.** It ranged from −0.58¢ to +0.49¢ per basket.
- **Spread** is taken live from every bid/ask.
- **Swap** comes from MT5 per position. cTrader FIX doesn't report swap.

### Two risk-limit bugs fixed

1. **Max orders now really caps the depth of a range.** Before, it counted the orders *pending at the same time*. The bot keeps only one pending at a time, so a range could go deeper than Max orders. Only Max lot stopped it, and in "+0.01" mode nothing did. Now level *n* is placed only if *n* ≤ Max orders.
2. **A basket stopped by a limit is still managed.** When Max orders, Max lot or Max basket loss stopped new levels, the engine used to stop watching the basket completely. The open positions then no longer closed at the target or the stop loss. Now they do; lock & pause and the audit keep working too.

### The app

- **Menu:** one menu icon at the top. It opens on hover and closes after you choose; the page uses the full width.
- **Risk & capital page** (`310a27b`):
  - **Account cards** at the top of the **Positions** page (the first page): balance, equity, margin used, free margin and margin level. They come from MT5; on cTrader they use the balance you type in. The Risk & capital page repeats them in one line.
  - **One place for every risk setting.** Lot sizing and loss limits moved here from Settings, so no setting appears on two pages.
  - **On MT5, the fields the broker already reports are hidden.** These are balance, leverage, hedged margin, account type and commission; the cards show the broker's values instead.
  - **Risk table fixes:** the guard marker, the units (¢ / $) and the commission now follow MT5.

### How each basket ended (History and the PDF report)

Every basket now carries a label saying how it ended, in the same words in the app and in the report:

| Label | Colour | Meaning |
|---|---|---|
| Profit target | green | Straight to the target; the price never turned against the first position. |
| Loss recovered -> profit target | blue | The basket went into loss (a second level filled) and still closed at the full target. |
| Loss recovered -> break-even exit | blue | It closed on the lowered break-even target (from level N). |
| Guard used -> closed in profit / Guard: lock -> back to break-even | blue | The margin guard held a level, or a lock froze the loss, and the basket still closed at or above 0. |
| Stop loss / Closed by you - in loss / ... - in loss | red | Closed in loss, with the reason. |

Under the label, "On the way" lists what acted: margin guard held a level, locked at −X, escape tries, the break-even target, the deepest point. The report's overview counts them: profit target, loss recovered, closed in loss, guard used.

Baskets saved before this change have no guard or lock information. A basket with two or more fills that reached its target still shows as "loss recovered".

### Closing a deep basket fast (1 Oct)

The VPS report of 1 Oct shows the problem this fixes. Basket #116 reached level 10 (5.12 lot last), and the break-even target fired. But it booked −166.68 USC.

Why: its 10 positions were closed one after another, each waiting for the broker's answer (round trip about 0.46 s on average on the VPS). That is about 4–5 seconds, with up to 3+ lots exposed. The price moved 0.6–1.2 against it in that time.

Now:
- **MT5:** the bridge sends every close without waiting (`OrderSendAsync`), so a basket's closes reach the broker together. A refused close is still reported to the app.
- **Close order:** the positions close in the order that keeps the open exposure smallest. The big net-side leg goes first, then the hedged pairs. Market movement during the rest barely matters.
- **Report:**
  - a cent account's amounts are now in USC, not "$";
  - the Settings section shows lot growth (×2), break-even target, lock at loss, margin guards and escape.

### Entry timeout (2 Oct)

Settings → **Entry timeout (s)**, default **120**. When a new range's first order hasn't triggered after this long, the order is cancelled and the range opens again around the live price. This happens when the price walked away from it.

- It only applies while nothing has filled.
- It waits during a market pause or a spread spike.
- 0 turns it off.

### Escape from a lock: what happened on 1 Oct (basket #165)

The basket was locked at −70.56 USC after 6 levels (lock BUY 0.21). Escape was switched on with 2 tries.

| Time | Event | Frozen loss |
|---|---|---|
| 19:41:29 | Lock BUY 0.21 | −70.56 |
| 19:41:30 | Escape stops placed: BUY 4177.69 / SELL 4175.09, 0.44 lot each | |
| 19:43:06 | SELL 0.44 filled at 4175.09 | |
| 19:43:08 | Price back inside the range (above 4175.39): re-lock BUY 0.44 | −103.51 |
| 19:43:09 | Escape stops again, 0.60 lot each (sized for the bigger loss) | |
| 19:43:13 | SELL 0.60 filled at 4175.09 | |
| 19:43:19 | Back inside the range: re-lock BUY 0.60 | −141.31 |
| 19:51:22 | Closed by hand | −140.00 |

Each try was a whipsaw. The price broke 0.30 below the range, came straight back within 2–6 seconds, and the re-lock bought back higher. Each try also added a spread on the bigger book.

This is the risk the setting warns about: escape is a bet on a breakout. In a sideways market it adds to the loss. **Keep it off** unless the market is clearly trending.

### Layout

- **No more cut-off text.** Every page wraps its text at the right edge, and the History rows put their facts on a second line when there is no room.
- **The costs under the P/L bar are folded by default.** One line shows the net and the total cost; a click opens the breakdown (gross, slippage, spread, commission, swap).
- **Admin badge** at the top right: an avatar with the user's name and an ADMIN tag. The name comes from Windows unless `userName` is set in `%APPDATA%\Muuk\desktop.json`. A `role` is kept there too, ready for more users with their own accounts later.

### The first MT5 run

JustMarkets demo cent, 30 Sep 15:30–17:15.

| | |
|---|---|
| Baskets | 23 in that window (24 by the evening), all closed at the profit target |
| Net | +34.51¢ over the 23 |
| Deepest | level 4 (0.15 lot) |
| Worst floating | −12.70¢ |
| News guard | paused 17:15–17:45 for USD Core PCE |

---

## 2. Where each setting lives

| Page | Settings |
|---|---|
| **Settings** | First order side, range width, profit target / auto target, entry offset, cooldowns, commission (cTrader only), market hours & news guard, account / platform |
| **Positions** | Account cards (balance, equity, margin, free margin, margin level), then the open positions |
| **Risk & capital** | Account (cTrader only), lot sizing, loss limits, break-even, margin guards, escape, worst-case table, reverse-trade calculator |
| Target gear (under the chart) | A shortcut to the same auto-target setting; it applies at once |

**Save & apply** writes the settings to the config file. It also hands them to the running bot:
- the target, loss limits and guards apply **at once**;
- the range and lots apply **from the next range**.

---

## 3. The account cards (Positions page)

| Card | Meaning |
|---|---|
| **Balance** | Money in the account without the open positions. |
| **Equity** | Balance + floating P/L: what the account is worth if everything closed now. |
| **Margin used** | What the broker holds for the open positions. |
| **Free margin** | Equity − margin: room for new positions. On MT5 it also shows about how many more lots fit. |
| **Margin level** | Equity ÷ margin × 100%. The guards act on this. Green is above both guards, amber is near them, red is under them. |

Example: equity 18,000¢ and margin 3,000¢ give a margin level of **600%**.

Brokers act at fixed levels:
- **margin call** at about 100% (JustMarkets: 40%);
- **stop-out** at 20–50%, where positions are closed by force.

The guards act long before that, at 300% and 200%.

---

## 4. Every Risk & capital setting

### Account (cTrader only; hidden on MT5)

| Setting | What it does |
|---|---|
| Account balance | cTrader FIX doesn't send the balance, so you type it in. The cards, table and guards use it. |
| Leverage | 1:x. It sets the margin per lot: **price × 100 oz ÷ leverage**. For example, 4,200 × 100 ÷ 500 = $840 per lot. |
| Hedged margin (%) | How a buy + sell pair is charged. 100 = both sides in full, which is the safe choice. |
| Account type | Standard ($) or cent (USC). On MT5 it is detected automatically. |

### Lot sizing (applies from the next range)

| Setting | What it does | If you change it |
|---|---|---|
| **First lot** | Lot of the first order in each range. | Everything scales with it. 0.01 → 0.05 makes profit, loss and margin **5×**. |
| **Lot growth** | Safe (+increment), Balanced (×1.5), Aggressive (×2). | ×2 wins back a deep basket with one bounce, but the loss grows fastest. +0.01 grows slowly but needs a longer run to recover. |
| multiplier / increment | The ×2 or the +0.01 itself. | ×2 → ×1.5 halves the growth of the deep levels. |
| **Max orders** | The deepest level a range may reach. | The basket adds nothing more after this, but its positions still close at the target or the stop loss. On MT5 it is kept 2 under the broker's order limit when the broker reports one (JustMarkets Standard Cent: 30 positions). |
| **Max lot** | No single order is bigger than this. | With ×2, a max lot of 10 stops at level 11 (10.24 lot). |

**Example: price bouncing between the two lines.** Range $2, first lot 0.01. Loss at each fill, before spread (¢ on cent, $ on standard):

| Level | 2 | 4 | 6 | 8 | 10 |
|---|---|---|---|---|---|
| ×2: lot at this level | 0.02 | 0.08 | 0.32 | 1.28 | 5.12 |
| ×2: floating loss | −2 | −10 | −42 | −170 | −682 |
| ×2: move needed to be back at 0 | $2 | $2 | $2 | $2 | $2 |
| +0.01: lot at this level | 0.02 | 0.04 | 0.06 | 0.08 | 0.10 |
| +0.01: floating loss | −2 | −8 | −18 | −32 | −50 |
| +0.01: move needed to be back at 0 | $2 | $4 | $6 | $8 | $10 |

What the table shows:
- **×2:** any deep basket comes back to 0 after a clean $2 run, but the loss nearly **doubles every level**. On a 19,280¢ account, level 12 is about −2,730¢ and level 14 about −10,900¢.
- **+0.01:** the loss stays small, but the price must run further before the basket gets back to 0.
- **Range width** is on the Settings page. A **$4 range doubles every loss above**.

The **Worst case** table on the page shows these numbers for your real settings, spread, commission and margin.

### Loss limits (0 = off; they apply at once)

| Setting | What happens when the basket's net reaches −X |
|---|---|
| **Stop loss** | Everything is closed and every pending order cancelled. The loss is **taken**. With Auto-open on, a new range starts after the cooldown. |
| **Lock & pause at loss** | Pending orders are cancelled and one opposite order **freezes** the loss (buys = sells). The bot waits for you. The loss is not gone, and swap is charged every night. |
| **Max basket loss** | No new level is placed. The open positions stay and still close at the target (or the stop loss). |
| **Daily loss limit** | When today's booked + floating net reaches −X, everything is closed and no new range opens until tomorrow. |

**Example: first lot 0.01, ×2, balance 19,280¢.**
- **Stop loss 200** closes the basket around level 8 (−170, then −340). You lose about 200¢, and the account keeps 19,000+.
- **Lock at 200** freezes the basket at about −200 instead. You then decide: close it, or leave it for a break-even close.
- **Daily loss limit 1,000** means a bad day costs at most about 1,000¢ (5% of the account).
- **All four at 0 (the current setting):** only the margin guards stand between a long back-and-forth and the account.

### Break-even (a deep basket gets out at the first bounce)

| Setting | What it does |
|---|---|
| **Break-even from level** | From this fill on, the target drops to "close at net". 0 = off. |
| close at net | The net, after every cost, at which such a basket closes (default 0.50). |
| also when guarded | While a guard's lock stop waits, or a lock holds, close everything as soon as the net is back at "close at net". |

Example: from level 5, close at net 0.50.
- The normal target at level 5 is several ¢.
- From that level on, the basket closes as soon as the net is +0.50 after costs.
- It gives up a little profit so it doesn't wait for the price to turn again.

Lower values (for example level 3) get out sooner with smaller wins. 0 always waits for the full target.

### Margin guards

| Setting | What it does |
|---|---|
| **Hold the next level under (%)** | Before placing a level, the bot works out the margin level that fill would leave. Under this value, that level is **not** placed. A stop order for the net lots (a lock) waits at its line instead. Default **300%**. |
| **Lock at once under (%)** | If the margin level **now** falls under this, the basket is locked at market immediately. Default **200%**. |

Example with the 300% guard. The next level would leave a margin level of 280%, so a lock stop is placed instead.
- **Price reaches the line:** the lock fills and the loss is frozen, well before any margin call.
- **Price turns back first:** the basket closes at break-even.

Changing the values:
- **Higher (500% / 300%):** safer; the basket stops adding levels earlier.
- **Lower (200% / 120%):** more levels allowed, closer to the broker's margin call.
- **0:** off (not recommended).

### Escape from a lock (off by default)

Escape is a bet on a breakout. From a lock it places stop orders just outside the range, sized so that a run of the escape move wins the frozen loss back.

| Setting | Default | Meaning |
|---|---|---|
| escape move ($) | 2 | The run the escape trade is sized for. |
| beyond the range by ($) | 0.30 | How far outside the range lines the escape orders sit. |
| trail to entry after ($) | 1 | Once the escape has run this far, it locks again if the price comes back to its entry. |
| tries per basket | 2 | After this many failed escapes, the basket stays locked. |

Example:
- Locked at −100¢, net 0.05 lot short, escape move $2.
- Escape lot = net + (loss + cost) ÷ (move × 100) ≈ 0.05 + 0.50 = **0.55 lot**.
- A clean $2 run brings the basket back to about 0.
- If the price turns back instead, it locks again and **that try's cost is added** to the loss.

That is why escape is capped and off by default.

---

## 5. The two tools at the bottom of the page

- **Worst case, level by level.** For each level it shows:
  - the lot, the total lots, and the worst floating loss (with spread and commission);
  - the margin, the margin level and the equity left;
  - the move needed to be back at 0.

  Rows turn amber at a margin call and red at a stop-out. "guard: lock stop" marks the level the 300% guard holds back.
- **Reverse-trade calculator.** For the open positions (or a level of the table), it shows the opposite trade that:
  - **locks** the loss (move = 0), or
  - is back at 0 after a move of $X.

  It gives the lot, its cost, and the margin, free margin and margin level after it.

---

## 6. Costs, in one place

| Cost | Where it comes from |
|---|---|
| Spread | Live bid/ask on every quote. The running P/L is valued at bid (buys) / ask (sells). |
| Commission | **MT5:** learned from the account's deals, and every fill's real charge is booked. **cTrader:** Settings (IC Markets Raw = $30 per $1M, charged on open and again on close). |
| Swap | **MT5:** per position, from the broker. **cTrader:** not available over FIX. |
| Slippage | Entry = the Stop fill vs its price. Exit = what the closing fills booked vs the net at the trigger. Already inside the net P/L. |

**Auto target** = a percentage of what the open positions cost (commission + spread):
- level 20 = 200% (twice the cost), 40 = 240%, 60 = 280%, 80 = 320%, 100 = 360%;
- a higher level wins more per basket, but stays in the market longer.
