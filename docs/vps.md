# Running Muuk on a VPS

A VPS keeps the bot running 24/5 when your own PC is off. Both programs
run on one:

| Program | Use it when |
|---|---|
| `muuk-live.exe` (console) | Unattended, 24/5. No graphics needed, smallest footprint. |
| `muuk-desktop.exe` (window) | You want to watch or control it over Remote Desktop. |

Both use the same config file and trading logic. The desktop app also
saves basket history, reports and logs.

## 1. Pick the VPS

- **Windows** (Windows Server 2019/2022, or Windows 10/11). The build is
  Windows-native.
- **Close to the broker's cTrader servers.** For IC Markets these are in
  London; confirm with IC Markets, since this changes. Pick a London or
  Europe region. The desktop app's *System speed → Broker round trip*
  shows the latency you actually get. Aim for a few milliseconds instead
  of 100+.
- 2 vCPU and 2–4 GB RAM is plenty. The bot itself needs microseconds per
  quote.

## 2. Copy it over

Copy these files into one folder, e.g. `C:\Muuk\`:

- `build\engine\ctrader_fix\Release\muuk-live.exe`
- `build\app\desktop\Release\muuk-desktop.exe`
- your config (`ic_markets_demo.local.json`)

The Release builds link the MSVC runtime dynamically. If a program won't
start ("VCRUNTIME140.dll missing"), install the *Microsoft Visual C++
Redistributable (x64)* from Microsoft on the VPS.

`muuk-desktop` needs OpenGL 3.0. Many VPSs have no GPU. If the window
doesn't open (the console shows `GLFW error ... WGL`), use `muuk-live`
there, or put a software OpenGL `opengl32.dll` (Mesa llvmpipe) next to
`muuk-desktop.exe`.

## 3. Start automatically

Use Task Scheduler → *Create Task*:

- **Trigger:** *At startup* for `muuk-live`, or *At log on* for
  `muuk-desktop` (it needs a desktop session).
- **Action:**
  - `C:\Muuk\muuk-live.exe --credentials C:\Muuk\ic_markets_demo.local.json`, or
  - `C:\Muuk\muuk-desktop.exe --config C:\Muuk\ic_markets_demo.local.json --connect`
- **Start in:** `C:\Muuk`
- **Settings:** *If the task fails, restart every 1 minute*. Leave
  *Stop the task if it runs longer than* unchecked.

Instead of `--connect`, you can tick **Connect when the app opens** in
the app itself. After a restart the bot reconnects and carries on with the
running basket. It checks the basket against the broker first; see
`docs/ctrader-fix.md`, "Persistence".

For `muuk-desktop` on a server that restarts by itself, enable automatic
logon for that user (Sysinternals *Autologon*). Otherwise the *At log on*
trigger never fires.

## 4. Keep it running

- **Power:** High performance plan; never sleep or hibernate.
- **Remote Desktop:** when you're done, close the RDP window
  (*disconnect*). **Don't sign out.** Signing out ends the session and
  closes the app. The desktop app treats that like a shutdown (see
  below).
- **Windows Update:** set active hours and pause automatic restarts.
  Restart by hand at the weekend, when gold (XAUUSD) is closed, roughly
  Friday 22:00 to Sunday 23:00 UTC.

## 5. What the exit guard covers, and what it doesn't

| Event | What happens |
|---|---|
| `muuk-live`: Ctrl+C, closing the console, logoff, shutdown | Cancels every pending order, closes every position, waits for the broker to confirm, then exits. `--keep-open-on-exit` turns this off. |
| `muuk-desktop`: close button / Alt+F4 with trades open | Asks: *Close everything & exit*, *Exit, keep them open*, or *Cancel*. |
| `muuk-desktop`: Windows logoff / shutdown / restart | Closes everything automatically. Windows shows "Muuk is closing open positions" for up to 8 s. |
| Crash, killed process (Task Manager), power or network loss, VPS host failure | **Nothing on the client can react.** Positions stay open at the broker, unmanaged. |

The last row is the real risk of any client-side bot. To limit it:

- Set **Max basket loss** in Settings. The bot then stops adding orders
  once the loss reaches that amount (it does not close the basket; see
  `docs/architecture.md`).
- The Task Scheduler restart (step 3) brings the bot back. When it
  reconnects, it checks the saved basket against the broker and carries
  on with it, including any order that filled while it was down. If
  something changed that it can't explain, it doesn't resume: it cancels
  the basket's pending orders, lists the remaining positions, and opens
  nothing new until they're closed.
- Check the account from the cTrader mobile app now and then.

## Before using real money

This is built and tested for the IC Markets **demo** account. Before a
live account:

- **TLS.** The FIX connection is plain TCP today (see
  `docs/ctrader-fix.md`, "Known limitations"), so your FIX password
  crosses the network unencrypted. cTrader offers SSL FIX ports; adding
  TLS is the one must-do.
- **Commission.** Set your real rate in Settings (it defaults to $0).
  Otherwise net P/L and the profit target ignore it.
- Run it on the demo account, on the VPS, for at least a week or two.
  Compare the PDF report with cTrader's own history.

## Keeping an eye on it

Everything the bot does is in its own files next to the config:

- `muuk_state/`: the running basket, so a restart carries on with it
- `muuk_history/baskets.jsonl`: every basket
- `muuk_reports/`: PDF reports
- `muuk_logs/`: the running log, plus copies saved on every disconnect

Copy these off the VPS, or open the desktop app over RDP to see the same
thing live.
