# Muuk

Ultra-fast XAUUSD pending-order strategy engine. See
`XAUUSD_Trading_Investment_Platform_Proposal.pdf` for the full product
vision; this repo currently implements **Phase 1** of that proposal's
roadmap (§20): the mathematical strategy engine (`engine/core`) and a
tick-data backtester (`engine/backtester`) that drives it. Broker
connectivity, the web platform, and everything else described in the
proposal come in later phases.

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

## Layout

```
engine/core/         strategy state machine — pure logic, no I/O, no heap
                      allocation in the decision path
engine/backtester/    historical tick replay: CSV loader, fill simulator,
                      stats aggregation, CLI
docs/                 architecture notes and the backtester's data format
```
