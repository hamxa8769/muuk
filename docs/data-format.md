# Backtester tick data format

`TickDataSource::loadCsv` reads a plain CSV file, one row per price update:

```
timestamp_ms,bid,ask
1700000000000,4336.49,4336.51
1700000001000,4337.09,4337.11
```

- **timestamp_ms**: integer, milliseconds since Unix epoch. Rows must be
  sorted ascending by timestamp — the loader throws `TickDataSourceError`
  otherwise.
- **bid / ask**: decimal price in the instrument's normal quoted form (e.g.
  `4337.11`, not scaled/integer ticks — that conversion happens internally
  against the `SymbolSpec` in your strategy config).
- A header row is optional and auto-detected (if the first row doesn't
  parse as `int,double,double` it's treated as a header and skipped).
- No volume/depth column — this strategy only needs bid/ask to detect Buy
  Stop / Sell Stop triggers and to mark open positions.

A small worked-example fixture that reproduces the six-order sequence from
the proposal's §3 table is checked in at
`engine/backtester/tests/fixtures/sample_ticks.csv`, alongside a matching
strategy config at `engine/backtester/tests/fixtures/sample_config.json`.
Run it directly:

```
build/engine/backtester/Release/muuk-backtest.exe ^
  --data engine/backtester/tests/fixtures/sample_ticks.csv ^
  --config engine/backtester/tests/fixtures/sample_config.json
```

## Getting real XAUUSD data

No historical data ships with this repo yet. To backtest against real
market conditions, export tick or 1-second bar data for XAUUSD from your
broker/platform (e.g. MetaTrader 5's history export) into this CSV shape.
Whatever the source, note the actual `digits`/`tickSize`/`tickValue`/
`contractSize`/volume constraints for the exact symbol and broker you
exported from, and put them in the `symbol` section of your strategy config
— the engine's precision model depends on those being accurate (see
`docs/architecture.md`, "Precision model").
