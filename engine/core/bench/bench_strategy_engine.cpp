#include <benchmark/benchmark.h>

#include "muuk/core/strategy_engine.hpp"

using namespace muuk::core;

namespace {

SymbolSpec xauusdSpec() {
    SymbolSpec spec;
    spec.digits = 2;
    spec.tickSize = 0.01;
    spec.tickValue = 1.0;
    spec.contractSize = 100.0;
    spec.volumeStep = 0.01;
    spec.volumeMin = 0.01;
    spec.volumeMax = 100.0;
    return spec;
}

StrategyConfig makeConfig(const SymbolSpec& spec) {
    StrategyConfig cfg;
    cfg.symbol = spec;
    cfg.range = RangeConfig{Price::fromDouble(4336.00, spec), Price::fromDouble(4337.00, spec)};
    cfg.lotProgression = LotProgressionConfig{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};
    cfg.riskLimits.maxOrders = 1'000'000;
    cfg.riskLimits.maxLot = Volume::fromDouble(1000.0, spec);
    cfg.basketProfitTargetMoney = 0.0; // never trips in this benchmark
    return cfg;
}

}  // namespace

// Steady-state decision latency: no fills, just price ticks arriving with
// the engine Active and evaluating (a no-op) basket P/L each time. This is
// the dominant hot path in live operation between triggers.
static void BM_OnTick_SteadyState(benchmark::State& state) {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(makeConfig(spec));
    engine.start();

    TickEvent tick;
    tick.bid = Price::fromDouble(4336.50, spec);
    tick.ask = Price::fromDouble(4336.52, spec);

    for (auto _ : state) {
        CommandBatch out = engine.onTick(tick);
        benchmark::DoNotOptimize(out);
        tick.bid.ticks += (tick.bid.ticks % 2 == 0) ? 1 : -1; // avoid a fully constant input
        tick.ask.ticks = tick.bid.ticks + 2;
    }
}
BENCHMARK(BM_OnTick_SteadyState);

// Trigger-path latency: calculate → validate → risk-check → emit the next
// pending order, exactly the pipeline in proposal §4. Each iteration fills
// the currently-armed order and measures the cost of immediately arming
// the next one in the sequence.
static void BM_OnOrderConfirmed_Rearm(benchmark::State& state) {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(makeConfig(spec));
    CommandBatch initial = engine.start();
    OrderId nextId = std::get<PlaceOrderCommand>(initial[0]).id;
    Price fillPrice = std::get<PlaceOrderCommand>(initial[0]).price;

    for (auto _ : state) {
        CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{nextId, fillPrice});
        benchmark::DoNotOptimize(out);
        nextId = std::get<PlaceOrderCommand>(out[0]).id;
        fillPrice = std::get<PlaceOrderCommand>(out[0]).price;
    }
}
BENCHMARK(BM_OnOrderConfirmed_Rearm);

BENCHMARK_MAIN();
