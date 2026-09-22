#include <catch2/catch_test_macros.hpp>

#include "muuk/core/risk.hpp"

using namespace muuk::core;

TEST_CASE("evaluateRisk: no limits breached returns None") {
    RiskLimits limits;
    limits.maxOrders = 10;
    limits.maxLot = Volume{1000};
    limits.maxBasketLossMoney = 500.0;
    limits.maxDrawdownMoney = 500.0;
    limits.maxMarginUsageFraction = 0.8;
    limits.maxSpreadTicks = 50;
    limits.maxSlippageTicks = 20;

    RiskCheckInput input;
    input.activeOrderCount = 2;
    input.nextOrderVolume = Volume{10};
    input.basketLossMoney = 10.0;
    input.drawdownMoney = 10.0;
    input.marginUsageFraction = 0.1;
    input.currentSpreadTicks = 5;
    input.estimatedSlippageTicks = 2;

    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::None);
}

TEST_CASE("evaluateRisk: max orders") {
    RiskLimits limits;
    limits.maxOrders = 3;
    RiskCheckInput input;
    input.activeOrderCount = 3;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxOrders);
}

TEST_CASE("evaluateRisk: max lot") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxLot = Volume{50};
    RiskCheckInput input;
    input.activeOrderCount = 0;
    input.nextOrderVolume = Volume{51};
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxLot);
}

TEST_CASE("evaluateRisk: max basket loss") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxBasketLossMoney = 100.0;
    RiskCheckInput input;
    input.basketLossMoney = 150.0;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxBasketLoss);
}

TEST_CASE("evaluateRisk: max drawdown") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxDrawdownMoney = 200.0;
    RiskCheckInput input;
    input.drawdownMoney = 250.0;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxDrawdown);
}

TEST_CASE("evaluateRisk: max margin usage") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxMarginUsageFraction = 0.5;
    RiskCheckInput input;
    input.marginUsageFraction = 0.9;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxMargin);
}

TEST_CASE("evaluateRisk: max spread") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxSpreadTicks = 30;
    RiskCheckInput input;
    input.currentSpreadTicks = 31;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxSpread);
}

TEST_CASE("evaluateRisk: max slippage") {
    RiskLimits limits;
    limits.maxOrders = 100;
    limits.maxSlippageTicks = 10;
    RiskCheckInput input;
    input.estimatedSlippageTicks = 11;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::MaxSlippage);
}

TEST_CASE("evaluateRisk: a zero/default limit means unconstrained") {
    RiskLimits limits; // maxOrders defaults to 10 in the struct; zero out the rest
    limits.maxOrders = 0; // 0 disables the check per evaluateRisk's contract
    RiskCheckInput input;
    input.activeOrderCount = 1000000;
    REQUIRE(evaluateRisk(limits, input) == RiskLimitKind::None);
}
