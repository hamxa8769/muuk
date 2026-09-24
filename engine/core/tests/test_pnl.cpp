#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/pnl.hpp"

using namespace muuk::core;

namespace {
SymbolSpec xauusdSpec() {
    SymbolSpec spec;
    spec.digits = 2;
    spec.tickSize = 0.01;
    spec.tickValue = 1.0; // $1 per tick per 1.0 lot, to keep expected numbers simple
    spec.contractSize = 100.0;
    spec.volumeStep = 0.01;
    spec.volumeMin = 0.01;
    spec.volumeMax = 100.0;
    return spec;
}
}  // namespace

TEST_CASE("positionPnlMoney: long position") {
    const SymbolSpec spec = xauusdSpec();
    const Price entry = Price::fromDouble(4337.00, spec);
    const Price mark = Price::fromDouble(4338.00, spec); // +1.00 = +100 ticks
    const Volume vol = Volume::fromDouble(0.10, spec);

    const double pnl = positionPnlMoney(OrderSide::BuyStop, vol, entry, mark, spec);
    REQUIRE(pnl == Catch::Approx(100.0 * 1.0 * 0.10));
}

TEST_CASE("positionPnlMoney: short position profits on a down move") {
    const SymbolSpec spec = xauusdSpec();
    const Price entry = Price::fromDouble(4336.00, spec);
    const Price mark = Price::fromDouble(4335.50, spec); // -0.50 = -50 ticks, short profits
    const Volume vol = Volume::fromDouble(0.02, spec);

    const double pnl = positionPnlMoney(OrderSide::SellStop, vol, entry, mark, spec);
    REQUIRE(pnl == Catch::Approx(50.0 * 1.0 * 0.02));
}

TEST_CASE("aggregatePositionPnlMoney matches summing positionPnlMoney over each fill") {
    const SymbolSpec spec = xauusdSpec();
    const Price mark = Price::fromDouble(4340.00, spec);

    struct Fill { Price entry; Volume volume; };
    const Fill fills[] = {
        {Price::fromDouble(4337.00, spec), Volume::fromDouble(0.01, spec)},
        {Price::fromDouble(4337.00, spec), Volume::fromDouble(0.03, spec)},
        {Price::fromDouble(4337.00, spec), Volume::fromDouble(0.05, spec)},
    };

    double expectedSum = 0.0;
    int64_t totalVolumeSteps = 0;
    int64_t sumEntryPriceTicksVolume = 0;
    for (const auto& fill : fills) {
        expectedSum += positionPnlMoney(OrderSide::BuyStop, fill.volume, fill.entry, mark, spec);
        totalVolumeSteps += fill.volume.steps;
        sumEntryPriceTicksVolume += fill.entry.ticks * fill.volume.steps;
    }

    const double aggregated = aggregatePositionPnlMoney(OrderSide::BuyStop, totalVolumeSteps,
                                                          sumEntryPriceTicksVolume, mark, spec);
    REQUIRE(aggregated == Catch::Approx(expectedSum));
}

TEST_CASE("aggregatePositionPnlMoney: short side sign matches positionPnlMoney") {
    const SymbolSpec spec = xauusdSpec();
    const Price entry = Price::fromDouble(4336.00, spec);
    const Price mark = Price::fromDouble(4335.00, spec); // short profits on a down move
    const Volume vol = Volume::fromDouble(0.04, spec);

    const double single = positionPnlMoney(OrderSide::SellStop, vol, entry, mark, spec);
    const double aggregated =
        aggregatePositionPnlMoney(OrderSide::SellStop, vol.steps, entry.ticks * vol.steps, mark, spec);
    REQUIRE(aggregated == Catch::Approx(single));
}

TEST_CASE("computeBasketPnl applies the proposal section 6 formula exactly") {
    BasketPnlInput input;
    input.grossPnlMoney = 42.18;
    input.totalVolumeLots = 0.21;
    input.spreadCostMoney = 1.05;
    input.swapMoney = 0.30;
    input.slippageMoney = 0.21;

    CostInputs costs;
    costs.commissionPerLotRoundTurn = 7.0; // -> commission = 0.21 * 7.0 = 1.47

    const BasketPnlResult result = computeBasketPnl(input, costs);
    REQUIRE(result.commissionMoney == Catch::Approx(1.47));
    const double expectedNet = 42.18 - 1.47 - 1.05 - 0.30 - 0.21;
    REQUIRE(result.netPnlMoney == Catch::Approx(expectedNet));
    REQUIRE(result.netPnlMoney == Catch::Approx(39.15).epsilon(0.001));
}

TEST_CASE("cTrader commission: $30 per $1M each side, rounded to the cent, charged for open and close") {
    const SymbolSpec spec = xauusdSpec();
    CostInputs costs;
    costs.commissionPerMillionPerSide = 30.0;
    // The broker's own figure: 0.32 lot of XAUUSD at ~4270.8 shows Commission -8.20 in cTrader.
    // One side = 0.32 x 100 oz x 4270.80 x 30 / 1,000,000 = 4.0999 -> 4.10; open + close = 8.20.
    REQUIRE(roundTurnCommissionMoney(0.32, 4270.80, spec, costs) == Catch::Approx(8.20));
    // 0.01 lot at 4270.00: 0.1281 -> 0.13 per side.
    REQUIRE(roundTurnCommissionMoney(0.01, 4270.00, spec, costs) == Catch::Approx(0.26));
    // 1 lot at 4270.00: 12.81 per side.
    REQUIRE(roundTurnCommissionMoney(1.00, 4270.00, spec, costs) == Catch::Approx(25.62));
    // A flat per-lot rate is used only when no per-million rate is set.
    costs.commissionPerMillionPerSide = 0.0;
    costs.commissionPerLotRoundTurn = 7.0;
    REQUIRE(roundTurnCommissionMoney(0.32, 4270.80, spec, costs) == Catch::Approx(2.24));
}
