#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/backtester/backtest_runner.hpp"

using namespace muuk::core;
using namespace muuk::backtester;

namespace {

SymbolSpec xauusdSpec() {
    SymbolSpec spec;
    spec.name = "XAUUSD";
    spec.digits = 2;
    spec.tickSize = 0.01;
    spec.tickValue = 1.0;
    spec.contractSize = 100.0;
    spec.volumeStep = 0.01;
    spec.volumeMin = 0.01;
    spec.volumeMax = 100.0;
    return spec;
}

BacktestConfig baseConfig(const SymbolSpec& spec) {
    BacktestConfig cfg;
    cfg.strategy.symbol = spec;
    cfg.strategy.range = RangeConfig{Price::fromDouble(4336.00, spec), Price::fromDouble(4337.00, spec)};
    cfg.strategy.lotProgression =
        LotProgressionConfig{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};
    cfg.strategy.riskLimits.maxOrders = 100;
    cfg.strategy.riskLimits.maxLot = Volume::fromDouble(10.0, spec);
    cfg.strategy.basketProfitTargetMoney = 0.0;
    return cfg;
}

// Oscillates through the range boundaries enough times to trigger the full
// six-order sequence from the proposal §3 worked example (same scenario as
// engine/backtester/tests/fixtures/sample_ticks.csv).
std::vector<TickRecord> sixFillOscillation() {
    return {
        {0, 4336.49, 4336.51},
        {1000, 4337.09, 4337.11}, // triggers seq1 Buy Stop
        {2000, 4335.89, 4335.91}, // triggers seq2 Sell Stop
        {3000, 4337.59, 4337.61}, // triggers seq3 Buy Stop
        {4000, 4335.39, 4335.41}, // triggers seq4 Sell Stop
        {5000, 4337.99, 4338.01}, // triggers seq5 Buy Stop
        {6000, 4334.99, 4335.01}, // triggers seq6 Sell Stop
    };
}

}  // namespace

TEST_CASE("BacktestRunner drives the full 6-order sequence from the proposal worked example") {
    const SymbolSpec spec = xauusdSpec();
    BacktestConfig cfg = baseConfig(spec); // basketProfitTargetMoney == 0 -> never closes early

    BacktestRunner runner(cfg);
    const BacktestReport report = runner.run(sixFillOscillation());

    REQUIRE(report.ticksProcessed == 7);
    REQUIRE(report.ordersFilled == 6);
    // 2 initial orders; the first fill's opposite is already pending (no
    // new order), then one re-arm per fill for the remaining 5 fills = 7.
    REQUIRE(report.ordersPlaced == 7);
    REQUIRE(report.basketsClosed == 0);
    REQUIRE(report.riskHalts == 0);
}

TEST_CASE("BacktestRunner closes the basket once the profit target is reached and reports net P/L") {
    const SymbolSpec spec = xauusdSpec();
    BacktestConfig cfg = baseConfig(spec);
    cfg.strategy.basketProfitTargetMoney = 1.0; // low bar, cleared by the rally below

    std::vector<TickRecord> ticks = {
        {0, 4336.49, 4336.51},
        {1000, 4337.09, 4337.11}, // triggers seq1 Buy Stop (long 0.01 lot @ 4337.00)
        // Rally: also re-triggers the freshly re-armed seq3 Buy Stop at the
        // same boundary (correct behaviour -- the strategy adds to the
        // position as price continues through the boundary), and clears
        // the $1 basket target on the same tick.
        {2000, 4341.99, 4342.01},
    };

    BacktestRunner runner(cfg);
    const BacktestReport report = runner.run(ticks);

    REQUIRE(report.ordersFilled >= 1);
    REQUIRE(report.basketsClosed == 1);
    REQUIRE(report.netPnlMoney > 0.0);
    REQUIRE(report.netPnlMoney >= cfg.strategy.basketProfitTargetMoney);
}

TEST_CASE("BacktestRunner halts and stops processing once a risk limit is breached") {
    const SymbolSpec spec = xauusdSpec();
    BacktestConfig cfg = baseConfig(spec);
    cfg.strategy.riskLimits.maxOrders = 1; // only the very first order is ever allowed

    BacktestRunner runner(cfg);
    const BacktestReport report = runner.run(sixFillOscillation());

    REQUIRE(report.riskHalts == 1);
    REQUIRE(report.ordersPlaced == 1);
    REQUIRE(report.ordersFilled == 0);
}
