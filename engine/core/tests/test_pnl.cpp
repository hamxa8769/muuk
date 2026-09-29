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

TEST_CASE("Auto target: a percentage of the cost, never below one cent") {
    REQUIRE(autoTargetMoney(0.36, 100.0) == Catch::Approx(0.36));
    REQUIRE(autoTargetMoney(0.36, 200.0) == Catch::Approx(0.72));
    REQUIRE(autoTargetMoney(0.50, 150.0) == Catch::Approx(0.75));
    REQUIRE(autoTargetMoney(0.0, 200.0) == Catch::Approx(0.01));  // nothing open yet
}

#include "muuk/core/risk_table.hpp"

TEST_CASE("Risk table: the worst case level by level, as in the discussion ($2 range, x2, 1:500)") {
    RiskTableInput in;
    in.spec = xauusdSpec();
    in.spec.tickValue = 1.0;
    in.lots = LotProgressionConfig{Volume::fromDouble(0.01, in.spec), Volume::fromDouble(0.01, in.spec), 2.0};
    in.costs.commissionPerMillionPerSide = 30.0;
    in.rangeWidth = 2.0;
    in.price = 4270.0;
    in.spread = 0.30;
    in.leverage = 500.0;
    in.levels = 10;
    const auto rows = riskTable(in);
    REQUIRE(rows.size() == 10);
    REQUIRE(rows[6].lot == Catch::Approx(0.64));
    REQUIRE(rows[6].worstLoss == Catch::Approx(155.0).margin(1.5));
    REQUIRE(rows[7].totalLots == Catch::Approx(2.55));
    REQUIRE(rows[7].worstLoss == Catch::Approx(312.0).margin(2.0));
    REQUIRE(rows[7].margin == Catch::Approx(1452.0).margin(1.0));
    REQUIRE(rows[7].recoveryMove == Catch::Approx(3.7).margin(0.1));
    // A max lot cuts the table off.
    in.maxLot = 0.30;
    REQUIRE(riskTable(in).size() == 5);  // 0.01 .. 0.16; 0.32 is above the limit
}

TEST_CASE("Margin rule: cTrader charges the larger side, MT5 the hedged part at its own rate") {
    // 1 lot buy + 1 lot sell at 4000, 1:500 -> one side = $800.
    REQUIRE(bookMargin(1.0, 1.0, 4000.0, 100.0, MarginRule{500.0, 100.0}) == Catch::Approx(800.0));
    REQUIRE(bookMargin(1.0, 1.0, 4000.0, 100.0, MarginRule{500.0, 50.0}) == Catch::Approx(400.0));
    REQUIRE(bookMargin(1.0, 1.0, 4000.0, 100.0, MarginRule{500.0, 0.0}) == Catch::Approx(0.0));
    // 1.5 buy / 1 sell: the open 0.5 at full rate + the hedged 1 at its rate.
    REQUIRE(bookMargin(1.5, 1.0, 4000.0, 100.0, MarginRule{500.0, 100.0}) == Catch::Approx(1200.0));
    REQUIRE(bookMargin(1.5, 1.0, 4000.0, 100.0, MarginRule{500.0, 50.0}) == Catch::Approx(800.0));
    REQUIRE(marginLevelPercent(1000.0, 800.0) == Catch::Approx(125.0));
    REQUIRE(marginLevelPercent(1000.0, 0.0) > 1e8);
}

TEST_CASE("Reverse trade: the lot that brings a losing basket back to 0 after a set move") {
    // Buys 0.85 @ 4272, sells 0.42 @ 4270, price 4271: floating -$127, net +0.43.
    ReverseTradeInput in;
    in.spec = xauusdSpec();
    in.costs.commissionPerMillionPerSide = 30.0;
    in.margin = MarginRule{500.0, 100.0};
    in.buyLots = 0.85;
    in.sellLots = 0.42;
    in.floating = -127.0;
    in.equity = 1000.0 - 127.0;
    in.price = 4271.0;
    in.spread = 0.30;

    // A lock: just the net lots, the loss stays (plus the lock's own cost).
    in.move = 0.0;
    ReverseTrade lock = reverseTrade(in);
    REQUIRE(lock.lockOnly);
    REQUIRE_FALSE(lock.buy);  // long basket -> sell
    REQUIRE(lock.lot == Catch::Approx(0.43));
    REQUIRE(lock.marginAfter == Catch::Approx(0.85 * 100.0 * 4271.0 / 500.0));
    REQUIRE(lock.lossAfter < -127.0);

    // Back to 0 after a $2 move down: q = 0.43 + (127 + cost(q)) / 200.
    in.move = 2.0;
    ReverseTrade rev = reverseTrade(in);
    REQUIRE(rev.lot == Catch::Approx(1.48));
    const double net = rev.lot - 0.43;  // short after the trade
    REQUIRE(net * 100.0 * 2.0 >= 127.0 + rev.cost - 1.0);  // the move pays the loss and the trade's cost
    REQUIRE(rev.marginAfter == Catch::Approx(1.90 * 100.0 * 4271.0 / 500.0).margin(1.0));  // about $1,623
    REQUIRE(rev.marginLevelAfter < 100.0);  // too much for $1000 standard
    // The same book on a 20,000 USC cent account has room.
    in.equity = 20000.0 - 127.0;
    REQUIRE(reverseTrade(in).marginLevelAfter > 1000.0);

    // Capped by the max lot.
    in.maxLot = 1.0;
    REQUIRE(reverseTrade(in).capped);
}

TEST_CASE("Risk table: margin level per row once a balance is given") {
    RiskTableInput in;
    in.spec = xauusdSpec();
    in.lots = LotProgressionConfig{Volume::fromDouble(0.01, in.spec), Volume::fromDouble(0.01, in.spec), 2.0};
    in.costs.commissionPerMillionPerSide = 30.0;
    in.price = 4270.0;
    in.levels = 9;
    in.balance = 1000.0;
    const auto rows = riskTable(in);
    // Fill 8: equity ~688 against $1,452 margin -> under 50 %, a stop-out.
    REQUIRE(rows[7].marginLevel == Catch::Approx((1000.0 - rows[7].worstLoss) / rows[7].margin * 100.0));
    REQUIRE(rows[7].marginLevel < 50.0);
    REQUIRE(rows[6].marginLevel > 100.0);
}
