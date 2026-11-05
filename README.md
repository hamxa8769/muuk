# Muuk

Muuk is a native Windows desktop bot that trades one strategy on gold (XAUUSD): a price range with stop orders on both sides, where each new level doubles the lot, until the whole basket closes at its target. It runs on **MetaTrader 5** (including cent accounts) or on **cTrader** over FIX.

> **Risk warning.** This is a martingale-style strategy: many small wins and occasional large losses. Backtests are simulations and don't promise live results. Test on a demo account first. Real money is traded only when you choose a LIVE account yourself.

## Install

1. Download or build `Muuk-Setup.exe` (see [Build the installer](#build-the-installer)) and run it. No admin rights are needed for Muuk itself.
2. Open Muuk from the desktop shortcut.
3. Click **Accounts** in the header, and pick or add your MT5 account.
4. Press **Connect**.

Before downloading anything, Setup scans the PC:

| Requirement | How it is found | If it is missing |
|---|---|---|
| Visual C++ runtime | Windows registry, version checked | `winget install --id Microsoft.VCRedist.2015+.x64 --exact --silent`, or Microsoft's `vc_redist.x64.exe /install /quiet /norestart` without winget |
| MetaTrader 5 (any broker's) | running terminals, MT5's data folders, the installed apps list, Program Files | MetaQuotes' `mt5setup.exe /auto`, if the box is ticked |

- Downloaded installers run only with a valid signature from Microsoft or MetaQuotes.
- Muuk goes to `%LOCALAPPDATA%\Programs\Muuk`, with desktop and Start menu shortcuts and an entry in Windows' installed apps list.
- Configs, history and reports live in `Documents\Muuk`. A reinstall or an uninstall never removes them.
- The installer isn't code-signed, so Windows SmartScreen may warn the first time (More info > Run anyway).

## What it does

- **Strategy:** range and stop orders, lots by a multiplier, fixed, automatic or last-lot profit target, and a break-even exit from a chosen level.
- **Smart entry:** range width from the one-minute ATR, plus an optional EMA trend filter and first order with the trend.
- **Protection:** lock at a level or at a loss, margin guards, a recovery escape, a daily loss limit, market-hours pauses and a news guard.
- **MT5 bridge:** Muuk installs its own EA and starts MT5 itself, trades only the config's account, and can switch MT5 to that account.
- **Recovery:** the running basket resumes after a restart or lost connection, and fills missed while offline are found and booked. Another program trading the account raises a warning.
- **Records:** basket history, volume and performance pages, an audit trail, PDF reports and logs.

## Documentation

| File | Contents |
|---|---|
| [docs/risk-and-settings.md](docs/risk-and-settings.md) | Every setting with examples, recent changes and backtest results |
| [docs/mt5.md](docs/mt5.md) | MetaTrader 5, the bridge and the accounts list |
| [docs/ctrader-fix.md](docs/ctrader-fix.md) | cTrader FIX: fields, protocol, persistence |
| [docs/vps.md](docs/vps.md) | Running around the clock on a Windows VPS |
| [docs/architecture.md](docs/architecture.md) | The event-driven core design |
| [docs/data-format.md](docs/data-format.md) | Backtester tick data and strategy config format |

## Build from source

Requirements: Windows, Visual Studio 2022 (or its Build Tools) with "Desktop development with C++", CMake 3.24 or newer, and internet on the first configure (dependencies come through CMake `FetchContent`).

```
cmake --preset default
cmake --build build --config Release
```

The app is `build\app\desktop\Release\muuk-desktop.exe`.

### Build the installer

```
powershell -ExecutionPolicy Bypass -File tools\build-installer.ps1
```

This writes `dist\Muuk-Setup.exe` using Windows' own IExpress, so no extra tool is needed. Close Muuk first, because a running exe can't be rebuilt. The installer never contains your configs: its first config is the example config, on demo, with no account in it.

### Tests

```
ctest --preset release
```

Run the runner tests on their own, because they depend on timing and can fail while a build is running.

### Other programs

| Program | Purpose |
|---|---|
| `muuk-live` | The bot without the GUI: `muuk-live --credentials my.local.json` |
| `muuk-backtest` | Tick-data backtester: `muuk-backtest --data ticks.csv --config strategy.json` |
| `muuk_core_bench` | Benchmarks of the engine's hot paths |

## Project layout

```
app/desktop/          the desktop app (Dear ImGui) and its icon
engine/core/          strategy state machine: pure logic, no I/O
engine/backtester/    historical tick replay, fill simulator, statistics
engine/ctrader_fix/   LiveRunner, FIX client, persistence, reports, market hours
engine/mt5_bridge/    gateway to the EA, MT5 launcher
engine/config_json/   shared strategy config parsing
tools/mt5/            MuukBridge.mq5, the EA built into the app
tools/installer/      Setup and uninstall scripts
tools/vps/            VPS checks and autostart
docs/                 the guides above
```

## Keep your accounts private

Account files are ignored by git: `*.local.json`, `muuk_history/`, `muuk_reports/`, `muuk_logs/`, `muuk_state/` and `dist/`. Never commit them, and don't share a VPS package made with `-Config`, because it contains your config.

Run only one Muuk per trading account. Two bots on one account cancel and close each other's orders.
