# Running Muuk on a VPS

A VPS keeps the bot running 24/5 when your own PC is off. It also puts
the bot next to the broker. From a home PC, reaching IC Markets' cTrader
servers can take 250+ ms. From a VPS in the same region it is a few ms.

| Program | Use it when |
|---|---|
| `muuk-desktop.exe` (window) | You want to watch and control it over Remote Desktop. Works without a graphics card. |
| `muuk-live.exe` (console) | Unattended, smallest footprint. |

Both use the same config, state and history files, and both reconnect by
themselves if the connection drops.

## 1. Pick the VPS

- **Windows**: Windows Server 2019/2022, or Windows 10/11.
- **Region: London or Europe.** IC Markets' cTrader servers are in London;
  confirm with IC Markets, since this can change.
- 2 vCPU and 2–4 GB RAM is plenty.
- No graphics card needed. The app then draws in software and switches to
  **Lite mode** by itself. Measured on 1 CPU core: about 2.5% with the
  window open and idle, about 0% when minimized, covered, or with Remote
  Desktop disconnected. RAM is ~40 MB open and ~1 MB minimized (memory is
  handed back to Windows).

### CPU

| | CPU | When to use |
|---|---|---|
| `muuk-desktop`, Lite mode | ~2.5% of a core idle, ~0% minimized | you want to look at it now and then |
| `muuk-desktop` while you move the mouse | ~30% of a core (6 frames/s) | only while you use it |
| `muuk-live` (console) | ~0% | least possible; watch the `[status]` line every minute |

- Lite mode means a flat look, no animations or graphs, and redrawing
  only when needed. Without a GPU: the numbers refresh every 2 s when
  idle, and 6 frames/s for 3 s after you touch it. Trading speed is the
  same in every mode.
- On a PC with a GPU you can switch it on too: Performance tab → *Lite
  mode*.
- **Run one copy only.** Two copies on one login keep kicking each other
  off the broker and burn CPU reconnecting. The bot now refuses to start a
  second copy for the same config ("Muuk is already running with this
  config"). That includes the auto-start task plus a copy you started by
  hand.

## 2. Make the package (on your PC)

```powershell
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package-vps.ps1 -Config .\ic_markets_demo.local.json
```

This creates `dist\Muuk-VPS\` and `dist\Muuk-VPS.zip` with:

| File | What it is |
|---|---|
| `muuk-desktop.exe`, `muuk-live.exe` | the app and the console bot |
| `msvcp140.dll`, `vcruntime140*.dll` | the Visual C++ runtime, so nothing needs installing |
| `check-vps.ps1` | checks the VPS is fit to trade (changes nothing) |
| `install-autostart.ps1` | starts the bot at logon and restarts it if it stops |
| `start-desktop.cmd`, `start-live.cmd` | double-click starters |
| `README-VPS.md` | this guide |
| your config | **it contains your FIX password** — keep the zip private |

## 3. Copy it to the VPS

1. Connect with Remote Desktop (`mstsc`). Before connecting, open *Show
   Options → Local Resources → More…* and tick your drive (e.g. `E:`).
   It then shows up on the VPS in File Explorer as *E on your-PC*.
2. Copy `Muuk-VPS.zip` to the VPS, e.g. into `C:\Muuk\`, and extract it.
   Copy-paste of the file over RDP also works.
3. If Windows blocks the files (downloaded or copied from another machine),
   right-click the zip → *Properties* → *Unblock*, before extracting.

## 4. Check the VPS

```powershell
cd C:\Muuk
powershell -ExecutionPolicy Bypass -File .\check-vps.ps1
```

| Line | Good | Fix if not |
|---|---|---|
| Broker trade / quote | under 20 ms | pick a VPS region nearer London |
| Time sync | recent sync | *Settings → Time & language → Sync now* |
| Power plan | sleep "never" | *Power options → never sleep* |
| Disk | 2 GB+ free | free up space |
| VC++ runtime | found | it's in the folder; re-extract the zip |

## 5. First run: check it works

1. Double-click `start-desktop.cmd`. The app opens and connects with your
   config.
2. **Performance tab.** Leave it open for a few minutes.
   - The top line should say **"Healthy - ready for trading"**.
   - Broker round trip should be a few ms (it's measured on every order).
   - The clock should be within a few hundred ms.
   - Quotes per second should be above 0 while the market is open.
   - *Graphics* shows whether it found a graphics card or is drawing in
     software (both are fine).
3. **Log tab.** Check for "logged on", "[sync] ...", and "engine is now
   active" or "connected -- waiting for Open range".
4. **Positions / Pending orders.** These must match what cTrader shows for
   the account.
5. **Reconnect test.** Disconnect the VPS network for ~30 s, or restart the
   app. The log shows `[reconnect] ...`, then the basket is picked up again
   (`[resume] ...`).
6. **Don't run the bot on your PC and the VPS at the same time** with the
   same login. The second logon kicks the first one off, and both then
   fight over the same basket. Close it on your PC first.

## 6. Start automatically

```powershell
powershell -ExecutionPolicy Bypass -File .\install-autostart.ps1
```

- This registers a Task Scheduler task. At logon it starts
  `muuk-desktop.exe --config <your config> --connect`, and restarts it if
  it stops.
- Use `-Mode live` for the console bot instead, and `-Remove` to undo.
- If the VPS can reboot by itself, turn on automatic logon for your user
  (Sysinternals *Autologon*). Otherwise nobody logs on and nothing starts.
- The app's own **Connect when the app opens** checkbox does the same
  thing as `--connect`.

## 7. Day to day

- **Remote Desktop:** close the RDP window (*disconnect*). **Don't sign
  out.** Signing out ends the session. The app treats that like a shutdown
  and closes every position first.
- **Minimize the app** when you're not watching it. Minimized (or with
  Remote Desktop disconnected) it draws nothing and uses ~0% CPU. Trading
  is never slowed by the window.
- **Windows Update:** set active hours and pause automatic restarts. Do
  restarts at the weekend, when gold (XAUUSD) is closed, roughly
  Friday 22:00 to Sunday 23:00 UTC.

## What protects the account, and what doesn't

| Event | What happens |
|---|---|
| Connection drops (network, broker restart) | Reconnects after 5, 10, 20, 40, then every 60 s. The running basket is checked against the broker and carries on. |
| App restarted, or the VPS rebooted | Starts again (step 6), reconnects, picks the basket up (`docs/ctrader-fix.md`, "Persistence"). |
| A pending order goes missing | Placed again, logged as `MISSED`. |
| `muuk-live`: Ctrl+C, closing the console, logoff, shutdown | Closes every position and pending order first (`--keep-open-on-exit` turns this off). |
| `muuk-desktop`: close button with trades open | Asks: close everything and exit, keep them open, or cancel. |
| `muuk-desktop`: Windows logoff / shutdown | Closes everything first (up to 8 s). |
| Crash, killed process, power or VPS host failure | **Nothing can react at that moment.** Positions stay open until the bot is back (auto-start). Set **Max basket loss** and check the account from the cTrader app. |

## Settings that protect the account

- **Stop loss ($):** closes the whole basket once its net loss reaches
  this amount, capping the loss per basket. 0 = off. Pick a value you
  can afford per basket before going live.
- **Max basket loss / Max orders / Max lot:** stop *adding* orders past
  these limits. The basket stays open, so pair them with a stop loss.
- **Spread limit:** a new range waits while the spread is too wide. A
  running basket always places its next order, whatever the spread.
- Orders the broker doesn't confirm within 8 s are checked with the
  broker and placed again if they're missing (`MISSED` in the log).

## Before using real money

- This is built and tested against the IC Markets **demo** account.
- **TLS:** the FIX connection is plain TCP today, so your FIX password
  crosses the network unencrypted. Add TLS before any live account.
- Set your real commission rate in Settings: "Commission ($ per $1M)" (IC Markets Raw = 30). Compare a position's Commission with cTrader's.
- Run on demo, on the VPS, for a week or two. Compare the PDF report with
  cTrader's own history.
