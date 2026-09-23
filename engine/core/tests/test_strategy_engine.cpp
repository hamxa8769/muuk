#include <cstddef>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/strategy_engine.hpp"

using namespace muuk::core;

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

StrategyConfig baseConfig(const SymbolSpec& spec) {
    StrategyConfig cfg;
    cfg.symbol = spec;
    cfg.range = RangeConfig{Price::fromDouble(4336.00, spec), Price::fromDouble(4337.00, spec)};
    cfg.lotProgression = LotProgressionConfig{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};
    cfg.riskLimits.maxOrders = 100;
    cfg.riskLimits.maxLot = Volume::fromDouble(10.0, spec);
    cfg.basketProfitTargetMoney = 0.0; // disabled unless a test overrides it
    cfg.maxRejectRetries = 3;
    return cfg;
}

const PlaceOrderCommand& placeCmd(const CommandBatch& batch, std::size_t index) {
    return std::get<PlaceOrderCommand>(batch[index]);
}

}  // namespace

TEST_CASE("start() arms the two boundary orders per the proposal worked example") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));

    const CommandBatch out = engine.start();
    REQUIRE(out.size() == 2);

    const auto& seq1 = placeCmd(out, 0);
    REQUIRE(seq1.sequenceIndex == 1);
    REQUIRE(seq1.side == OrderSide::BuyStop);
    REQUIRE(seq1.price == Price::fromDouble(4337.00, spec));
    REQUIRE(seq1.volume.toDouble(spec) == Catch::Approx(0.01));

    const auto& seq2 = placeCmd(out, 1);
    REQUIRE(seq2.sequenceIndex == 2);
    REQUIRE(seq2.side == OrderSide::SellStop);
    REQUIRE(seq2.price == Price::fromDouble(4336.00, spec));
    REQUIRE(seq2.volume.toDouble(spec) == Catch::Approx(0.02));

    REQUIRE(engine.status() == EngineStatus::Active);
    REQUIRE(engine.activeOrderCount() == 2);
}

TEST_CASE("fill sequence reproduces the full proposal table (sequences 1-6)") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));

    const CommandBatch initial = engine.start();
    const OrderId seq1Id = placeCmd(initial, 0).id;
    const OrderId seq2Id = placeCmd(initial, 1).id;

    struct Expected {
        int sequenceIndex;
        OrderSide side;
        double price;
        double lot;
    };
    const Expected expectedTable[] = {
        {3, OrderSide::BuyStop, 4337.00, 0.03},
        {4, OrderSide::SellStop, 4336.00, 0.04},
        {5, OrderSide::BuyStop, 4337.00, 0.05},
        {6, OrderSide::SellStop, 4336.00, 0.06},
    };

    // seq1 (Buy Stop) triggers -> re-arms seq3 (Buy Stop again).
    CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == expectedTable[0].sequenceIndex);
    REQUIRE(placeCmd(out, 0).side == expectedTable[0].side);
    REQUIRE(placeCmd(out, 0).price == Price::fromDouble(expectedTable[0].price, spec));
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(expectedTable[0].lot));
    const OrderId seq3Id = placeCmd(out, 0).id;

    // seq2 (Sell Stop) triggers -> re-arms seq4 (Sell Stop again).
    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == expectedTable[1].sequenceIndex);
    REQUIRE(placeCmd(out, 0).side == expectedTable[1].side);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(expectedTable[1].lot));
    const OrderId seq4Id = placeCmd(out, 0).id;

    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq3Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(placeCmd(out, 0).sequenceIndex == expectedTable[2].sequenceIndex);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(expectedTable[2].lot));

    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq4Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(placeCmd(out, 0).sequenceIndex == expectedTable[3].sequenceIndex);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(expectedTable[3].lot));
}

TEST_CASE("onOrderConfirmed for an unknown/duplicate id is a no-op") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));
    const CommandBatch initial = engine.start();
    const OrderId seq1Id = placeCmd(initial, 0).id;

    const CommandBatch bogus = engine.onOrderConfirmed(OrderConfirmedEvent{99999, Price::fromDouble(4337.00, spec)});
    REQUIRE(bogus.empty());

    const CommandBatch first = engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE_FALSE(first.empty());

    // Replaying the same confirmation must not re-trigger re-arming.
    const CommandBatch replay = engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(replay.empty());
}

TEST_CASE("basket closes and cancels remaining orders once the profit target is reached") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.basketProfitTargetMoney = 1.0; // low bar, comfortably cleared below
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const OrderId seq1Id = placeCmd(initial, 0).id;
    const OrderId seq2Id = placeCmd(initial, 1).id;

    // Buy Stop seq1 fills exactly at its requested price (no slippage).
    const CommandBatch afterFill =
        engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE_FALSE(afterFill.empty()); // re-armed seq3

    // Price rallies 5.00 -> long leg is comfortably in the money.
    TickEvent tick;
    tick.bid = Price::fromDouble(4341.99, spec);
    tick.ask = Price::fromDouble(4342.01, spec);
    const CommandBatch closeOut = engine.onTick(tick);

    REQUIRE(engine.status() == EngineStatus::Completed);
    bool sawClose = false;
    int cancelCount = 0;
    for (const auto& cmd : closeOut) {
        if (std::holds_alternative<CloseBasketCommand>(cmd)) {
            sawClose = true;
            REQUIRE(std::get<CloseBasketCommand>(cmd).netPnlMoney >= cfg.basketProfitTargetMoney);
        } else if (std::holds_alternative<CancelOrderCommand>(cmd)) {
            ++cancelCount;
        }
    }
    REQUIRE(sawClose);
    // seq2 (never filled) and the seq3 re-arm from the fill above are both
    // still pending and must be cancelled.
    REQUIRE(cancelCount == 2);
    REQUIRE(engine.activeOrderCount() == 0);
    (void)seq2Id;
}

TEST_CASE("risk engine halts new order creation once max orders is reached") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.riskLimits.maxOrders = 1; // only the very first order is allowed
    StrategyEngine engine(cfg);

    const CommandBatch out = engine.start();

    bool sawPlace = false;
    bool sawAlert = false;
    for (const auto& cmd : out) {
        if (std::holds_alternative<PlaceOrderCommand>(cmd)) sawPlace = true;
        if (std::holds_alternative<RiskAlertCommand>(cmd)) {
            sawAlert = true;
            REQUIRE(std::get<RiskAlertCommand>(cmd).kind == RiskLimitKind::MaxOrders);
        }
    }
    REQUIRE(sawPlace);
    REQUIRE(sawAlert);
    REQUIRE(engine.status() == EngineStatus::HaltedByRisk);
    REQUIRE(engine.activeOrderCount() == 1);
}

TEST_CASE("rejected orders retry up to the configured limit, then give up") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.maxRejectRetries = 2;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    OrderId currentId = placeCmd(initial, 0).id; // seq1, BuyStop
    const int initialCount = engine.activeOrderCount();

    // Retry #1: should re-arm with the same sequence index/side/price.
    CommandBatch out = engine.onOrderRejected(OrderRejectedEvent{currentId, RejectReason::BrokerRejected});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == 1);
    REQUIRE(placeCmd(out, 0).side == OrderSide::BuyStop);
    currentId = placeCmd(out, 0).id;

    // Retry #2 (2nd and final allowed retry).
    out = engine.onOrderRejected(OrderRejectedEvent{currentId, RejectReason::BrokerRejected});
    REQUIRE(out.size() == 1);
    currentId = placeCmd(out, 0).id;

    // Retry #3: retries exhausted, engine gives up silently on this side.
    out = engine.onOrderRejected(OrderRejectedEvent{currentId, RejectReason::BrokerRejected});
    REQUIRE(out.empty());

    // Only the untouched seq2 (Sell Stop) order remains armed.
    REQUIRE(engine.activeOrderCount() == initialCount - 1);
}

TEST_CASE("reArmOnFill=false takes a single fill and does not re-arm that side") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.reArmOnFill = false;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    REQUIRE(initial.size() == 2);
    const OrderId seq1Id = placeCmd(initial, 0).id;  // BuyStop
    const OrderId seq2Id = placeCmd(initial, 1).id;  // SellStop
    REQUIRE(engine.activeOrderCount() == 2);

    // seq1 (Buy Stop) fills: no re-arm, seq2 (Sell Stop) stays untouched.
    const CommandBatch afterFill =
        engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(afterFill.empty());
    REQUIRE(engine.activeOrderCount() == 1);
    REQUIRE(engine.status() == EngineStatus::Active);

    // The opposite side can still trigger independently.
    const CommandBatch afterSecondFill =
        engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(afterSecondFill.empty());
    REQUIRE(engine.activeOrderCount() == 0);
}
