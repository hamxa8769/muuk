# Muuk

Ultra-fast XAUUSD pending-order strategy engine. See
`XAUUSD_Trading_Investment_Platform_Proposal.pdf` for the full product
vision. This repo currently implements the proposal's roadmap (§20)
Phase 1 (the mathematical strategy engine, `engine/core`, and a tick-data
backtester, `engine/backtester`) and Phase 3 live demo execution
(`engine/ctrader_fix`, a FIX API client for IC Markets/cTrader). The web
platform and everything else described in the proposal come later.

Read `docs/architecture.md` first — it explains the event-driven design
that keeps the core engine allocation-free and identically testable in
both backtest and (future) live contexts.

## Prerequisites

- Windows with the MSVC toolchain (Visual Studio 2022 Build Tools or full
  Visual Studio, with the "Desktop development with C++" workload)
- CMake >= 3.24
- Internet access on first configure (dependencies — Catch2, Google
  Benchmark, nlohmann/json — are fetched via CMake `FetchContent`; no
  vcpkg/manual install required)

## Build

```
cmake --preset default
cmake --build build --config Release
```

## Run the tests

```
ctest --preset release
```

or run the test binaries directly for more verbose output:

```
build\engine\core\Release\muuk_core_tests.exe
build\engine\backtester\Release\muuk_backtester_tests.exe
```

## Run the benchmarks

```
build\engine\core\Release\muuk_core_bench.exe
```

This measures the two hot paths the proposal calls out explicitly: steady
-state tick handling (`BM_OnTick_SteadyState`) and the trigger-to-next
-order pipeline (`BM_OnOrderConfirmed_Rearm`, i.e. "detect trigger ->
immediately calculate next order").

## Run a backtest

```
build\engine\backtester\Release\muuk-backtest.exe ^
  --data engine\backtester\tests\fixtures\sample_ticks.csv ^
  --config engine\backtester\tests\fixtures\sample_config.json
```

See `docs/data-format.md` for the tick CSV schema and how to point this at
real exported broker data, and the strategy config JSON fields (symbol
spec, range, lot progression, risk limits, costs).

## Run live demo trading (IC Markets / cTrader FIX API)

1. Get FIX API credentials for a **demo** account from the cTrader
   platform: Settings → FIX API. See `docs/ctrader-fix.md` for what each
   field means.
2. Copy `engine/ctrader_fix/config.example.json` to a **local, gitignored**
   file (e.g. `my_ic_markets_demo.local.json` — the `*.local.json` pattern
   is already in `.gitignore`) and fill in your real host/port/BrokerUID/
   trader login/password. **Never commit this file.**
3. Run:
   ```
   build\engine\ctrader_fix\Release\muuk-live.exe --credentials my_ic_markets_demo.local.json
   ```

This connects both the Quote and Trade FIX sessions, resolves the
configured symbol to the broker's numeric id, arms the strategy's initial
pending orders, and streams live quotes into the same `StrategyEngine`
used by the backtester — see `docs/ctrader-fix.md` for the wire-protocol
details and current known limitations (no TLS yet, demo/plain-TCP only).

## Run the native desktop app

`app/desktop` (proposal §9, "Desktop Trading Application") is a live
status/control panel over the same `LiveRunner` the CLI uses:

```
build\app\desktop\Release\muuk-desktop.exe
```

Paste the path to your local credentials file (same one as `muuk-live`,
see above) into the "Credentials file" field and click **Start**. It shows
symbol, strategy status, range, active order count, live bid/ask/spread,
and basket gross/net P/L, plus a scrolling log of the same events
`muuk-live` prints to stderr. **Stop** disconnects cleanly.

## Layout

```
engine/core/         strategy state machine — pure logic, no I/O, no heap
                      allocation in the decision path
engine/backtester/    historical tick replay: CSV loader, fill simulator,
                      stats aggregation, CLI
engine/ctrader_fix/   live demo trading via IC Markets' cTrader FIX API
engine/config_json/   shared JSON parsing for StrategyConfig (used by both
                       the backtester and ctrader_fix CLIs)
app/desktop/          native ImGui status/control panel over LiveRunner
docs/                 architecture notes, backtester data format, FIX notes
```
