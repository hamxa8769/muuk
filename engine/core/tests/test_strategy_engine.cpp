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
    const OrderId seq1Id = placeCmd(initial, 0).id;  // Buy Stop @4337, 0.01
    const OrderId seq2Id = placeCmd(initial, 1).id;  // Sell Stop @4336, 0.02

    // seq1 (Buy Stop) fills: the opposite side (seq2, Sell Stop) is
    // already pending from the initial pair, so nothing new is armed yet.
    CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(out.empty());

    // seq2 (Sell Stop) fills: NOW the Buy side has nothing pending, so a
    // fresh Buy Stop (seq3) is armed -- matching the table's seq3 exactly.
    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == 3);
    REQUIRE(placeCmd(out, 0).side == OrderSide::BuyStop);
    REQUIRE(placeCmd(out, 0).price == Price::fromDouble(4337.00, spec));
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.03));
    const OrderId seq3Id = placeCmd(out, 0).id;

    // seq3 (Buy Stop) fills -> Sell Stop (seq4) armed.
    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq3Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == 4);
    REQUIRE(placeCmd(out, 0).side == OrderSide::SellStop);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.04));
    const OrderId seq4Id = placeCmd(out, 0).id;

    // seq4 (Sell Stop) fills -> Buy Stop (seq5) armed.
    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq4Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == 5);
    REQUIRE(placeCmd(out, 0).side == OrderSide::BuyStop);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.05));
    const OrderId seq5Id = placeCmd(out, 0).id;

    // seq5 (Buy Stop) fills -> Sell Stop (seq6) armed.
    out = engine.onOrderConfirmed(OrderConfirmedEvent{seq5Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).sequenceIndex == 6);
    REQUIRE(placeCmd(out, 0).side == OrderSide::SellStop);
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.06));
}

TEST_CASE("the same side never gets a duplicate pending order while its own is still outstanding") {
    // Regression test: an earlier version re-armed the side that just
    // filled instead of the opposite side, which let one side accumulate
    // unbounded fills whenever price kept moving one direction (caught
    // via live testing as "bulk orders on one side"). The corrected
    // design makes this structurally impossible: at most one order is
    // ever pending, and a fill always arms the opposite of whichever side
    // just triggered -- never the same one.
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));

    const CommandBatch initial = engine.start();
    const OrderId seq1Id = placeCmd(initial, 0).id;  // Buy Stop @4337
    const OrderId seq2Id = placeCmd(initial, 1).id;  // Sell Stop @4336

    // Buy fills first: opposite (Sell) already pending, nothing new armed.
    REQUIRE(engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)}).empty());
    REQUIRE(engine.activeOrderCount() == 1);  // only seq2 remains

    // Sell fills: opposite (Buy) now has nothing pending, so a fresh Buy
    // Stop is armed -- never another Sell Stop.
    const CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).side == OrderSide::BuyStop);
    REQUIRE(engine.activeOrderCount() == 1);
}

TEST_CASE("onOrderConfirmed for an unknown/duplicate id is a no-op") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));
    const CommandBatch initial = engine.start();
    const OrderId seq1Id = placeCmd(initial, 0).id;
    const OrderId seq2Id = placeCmd(initial, 1).id;

    const CommandBatch bogus = engine.onOrderConfirmed(OrderConfirmedEvent{99999, Price::fromDouble(4337.00, spec)});
    REQUIRE(bogus.empty());

    // seq1 (Buy) fills: opposite (Sell/seq2) already pending, nothing new.
    REQUIRE(engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)}).empty());

    // seq2 (Sell) fills: now a fresh order is armed on the opposite side.
    const CommandBatch first = engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE_FALSE(first.empty());

    // Replaying that same confirmation must not re-trigger re-arming.
    const CommandBatch replay = engine.onOrderConfirmed(OrderConfirmedEvent{seq2Id, Price::fromDouble(4336.00, spec)});
    REQUIRE(replay.empty());

    // Replaying the earlier (seq1) confirmation, now stale too, is also a no-op.
    const CommandBatch replaySeq1 =
        engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(replaySeq1.empty());
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
    // The opposite side (Sell Stop, seq2) is already pending, so nothing
    // new is armed yet.
    const CommandBatch afterFill =
        engine.onOrderConfirmed(OrderConfirmedEvent{seq1Id, Price::fromDouble(4337.00, spec)});
    REQUIRE(afterFill.empty());

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
            const auto& close = std::get<CloseBasketCommand>(cmd);
            REQUIRE(close.netPnlMoney >= cfg.basketProfitTargetMoney);
            // Only seq1 (Buy Stop, 0.01 lot) ever filled -> net long 0.01,
            // so the caller needs a closing Sell of that size (closeSide
            // reports the net *exposure* side being closed, i.e. BuyStop).
            REQUIRE(close.closeSide == OrderSide::BuyStop);
            REQUIRE(close.closeVolume.toDouble(spec) == Catch::Approx(0.01));
        } else if (std::holds_alternative<CancelOrderCommand>(cmd)) {
            ++cancelCount;
        }
    }
    REQUIRE(sawClose);
    // seq2 (never filled) is the sole still-pending order and must be cancelled.
    REQUIRE(cancelCount == 1);
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

TEST_CASE("seedOffsetPoints>0 arms two tiny seed orders around the range midpoint, not the full boundaries") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;  // range is 4336.00-4337.00, mid = 4336.50
    StrategyEngine engine(cfg);

    const CommandBatch out = engine.start();
    REQUIRE(out.size() == 2);

    const auto& seed1 = placeCmd(out, 0);
    REQUIRE(seed1.side == OrderSide::BuyStop);
    REQUIRE(seed1.price == Price::fromDouble(4336.55, spec));

    const auto& seed2 = placeCmd(out, 1);
    REQUIRE(seed2.side == OrderSide::SellStop);
    REQUIRE(seed2.price == Price::fromDouble(4336.45, spec));

    REQUIRE(engine.status() == EngineStatus::Active);
}

TEST_CASE("seedOffsetPoints>0: the Buy seed filling first anchors the upper boundary there") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const OrderId seedBuyId = placeCmd(initial, 0).id;   // BuyStop @ 4336.55
    const OrderId seedSellId = placeCmd(initial, 1).id;  // SellStop @ 4336.45

    const CommandBatch out =
        engine.onOrderConfirmed(OrderConfirmedEvent{seedBuyId, Price::fromDouble(4336.55, spec)});

    // The other seed is cancelled, and the *only* new order armed is the
    // opposite side (Sell Stop) at the derived lower boundary -- the Buy
    // side isn't re-armed until this Sell Stop eventually fills too.
    REQUIRE(out.size() == 2);
    bool sawCancel = false;
    bool sawNewSell = false;
    for (const auto& cmd : out) {
        if (std::holds_alternative<CancelOrderCommand>(cmd)) {
            REQUIRE(std::get<CancelOrderCommand>(cmd).id == seedSellId);
            sawCancel = true;
        } else if (const auto* p = std::get_if<PlaceOrderCommand>(&cmd)) {
            REQUIRE(p->side == OrderSide::SellStop);
            // Original width (4337.00-4336.00 = 1.00) preserved below the anchor.
            REQUIRE(p->price == Price::fromDouble(4335.55, spec));
            sawNewSell = true;
        }
    }
    REQUIRE(sawCancel);
    REQUIRE(sawNewSell);
    REQUIRE(engine.activeOrderCount() == 1);
}

TEST_CASE("seedOffsetPoints>0: the Sell seed filling first anchors the lower boundary there") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const OrderId seedBuyId = placeCmd(initial, 0).id;   // BuyStop @ 4336.55
    const OrderId seedSellId = placeCmd(initial, 1).id;  // SellStop @ 4336.45

    const CommandBatch out =
        engine.onOrderConfirmed(OrderConfirmedEvent{seedSellId, Price::fromDouble(4336.45, spec)});

    // Symmetric: only the opposite side (Buy Stop) gets a fresh order.
    REQUIRE(out.size() == 2);
    bool sawCancel = false;
    bool sawNewBuy = false;
    for (const auto& cmd : out) {
        if (std::holds_alternative<CancelOrderCommand>(cmd)) {
            REQUIRE(std::get<CancelOrderCommand>(cmd).id == seedBuyId);
            sawCancel = true;
        } else if (const auto* p = std::get_if<PlaceOrderCommand>(&cmd)) {
            REQUIRE(p->side == OrderSide::BuyStop);
            REQUIRE(p->price == Price::fromDouble(4337.45, spec));  // anchor + original width
            sawNewBuy = true;
        }
    }
    REQUIRE(sawCancel);
    REQUIRE(sawNewBuy);
    REQUIRE(engine.activeOrderCount() == 1);
}

TEST_CASE("seedOffsetPoints>0: after anchoring, the opposite-side alternation continues correctly") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const OrderId seedBuyId = placeCmd(initial, 0).id;  // BuyStop @ 4336.55

    // Anchor: the Buy seed fills -> a Sell Stop is armed at the derived
    // lower boundary (4335.55); the Buy side itself is NOT re-armed yet.
    const CommandBatch afterAnchor =
        engine.onOrderConfirmed(OrderConfirmedEvent{seedBuyId, Price::fromDouble(4336.55, spec)});
    OrderId anchoredSellId = kInvalidOrderId;
    for (const auto& cmd : afterAnchor) {
        if (const auto* p = std::get_if<PlaceOrderCommand>(&cmd); p && p->side == OrderSide::SellStop) {
            anchoredSellId = p->id;
        }
    }
    REQUIRE(anchoredSellId != kInvalidOrderId);
    REQUIRE(engine.activeOrderCount() == 1);

    // That Sell Stop fills -> the Buy side now has nothing pending, so a
    // fresh Buy Stop is armed back at the anchor level (4336.55) -- and
    // seeding must not fire a second time here.
    const CommandBatch afterSecondFill =
        engine.onOrderConfirmed(OrderConfirmedEvent{anchoredSellId, Price::fromDouble(4335.55, spec)});
    REQUIRE(afterSecondFill.size() == 1);
    REQUIRE(std::holds_alternative<PlaceOrderCommand>(afterSecondFill[0]));
    const auto& newBuy = std::get<PlaceOrderCommand>(afterSecondFill[0]);
    REQUIRE(newBuy.side == OrderSide::BuyStop);
    REQUIRE(newBuy.price == Price::fromDouble(4336.55, spec));
    // L(3): seq1 shared by both seeds (only one ever fills), seq2 the
    // anchor re-arm (the Sell Stop above), seq3 this one.
    REQUIRE(newBuy.volume.toDouble(spec) == Catch::Approx(0.03));
}

TEST_CASE("seedOffsetPoints>0: restart() re-seeds around the tick that triggered the close, not the stale prior range") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;
    cfg.basketProfitTargetMoney = 0.05;  // low bar, comfortably cleared below
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const OrderId seedBuyId = placeCmd(initial, 0).id;  // BuyStop @ 4336.55 (mid 4336.50 + 0.05)

    // Anchor: the Buy seed fills at 4336.55 -> range becomes [4335.55, 4336.55].
    engine.onOrderConfirmed(OrderConfirmedEvent{seedBuyId, Price::fromDouble(4336.55, spec)});

    // Price rallies well away from that anchor, clearing the profit target.
    TickEvent tick;
    tick.bid = Price::fromDouble(4349.99, spec);
    tick.ask = Price::fromDouble(4350.01, spec);
    engine.onTick(tick);
    REQUIRE(engine.status() == EngineStatus::Completed);

    // The new cycle's seeds must sit around this tick (bid 4349.99 / ask
    // 4350.01) -- not the now-stale anchored range from before (~4336.05)
    // -- so the strategy keeps following live price across cycles rather
    // than arming orders tens of dollars away from the market.
    const CommandBatch restartOut = engine.restart();
    REQUIRE(restartOut.size() == 2);
    REQUIRE(placeCmd(restartOut, 0).side == OrderSide::BuyStop);
    REQUIRE(placeCmd(restartOut, 0).price == Price::fromDouble(4350.06, spec));  // ask + 0.05
    REQUIRE(placeCmd(restartOut, 1).side == OrderSide::SellStop);
    REQUIRE(placeCmd(restartOut, 1).price == Price::fromDouble(4349.94, spec));  // bid - 0.05
}

TEST_CASE("seedOffsetPoints>0: seeds sit outside the spread, so they can't both trigger at once") {
    // Regression: seeds used to be placed at mid +/- offset. With a spread
    // wider than 2*offset (e.g. XAUUSD's ~0.11 vs a 0.05 offset) the Buy
    // Stop landed below the ask and the Sell Stop above the bid, so both
    // triggered immediately and the bot opened two 0.01 positions at once.
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.seedOffsetPoints = 0.05;
    StrategyEngine engine(cfg);

    TickEvent tick;
    tick.bid = Price::fromDouble(4280.88, spec);
    tick.ask = Price::fromDouble(4280.99, spec);  // spread 0.11
    REQUIRE(engine.onTick(tick).empty());  // Idle: only records the tick

    const CommandBatch out = engine.start();
    REQUIRE(out.size() == 2);
    const auto& buySeed = placeCmd(out, 0);
    const auto& sellSeed = placeCmd(out, 1);
    REQUIRE(buySeed.side == OrderSide::BuyStop);
    REQUIRE(sellSeed.side == OrderSide::SellStop);
    REQUIRE(buySeed.price.ticks > tick.ask.ticks);   // a Buy Stop triggers on ask >= price
    REQUIRE(sellSeed.price.ticks < tick.bid.ticks);  // a Sell Stop triggers on bid <= price
    REQUIRE(buySeed.volume.toDouble(spec) == Catch::Approx(0.01));
    REQUIRE(sellSeed.volume.toDouble(spec) == Catch::Approx(0.01));
}

TEST_CASE("basket P/L charges slippage exactly once, and net equals the real move from the actual fill") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.startSide = StartSide::BuyOnly;
    cfg.costs.commissionPerLotRoundTurn = 7.0;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    const auto& buy = placeCmd(initial, 0);  // Buy Stop 0.01 @ 4337.00
    // Fills 0.10 worse than requested.
    engine.onOrderConfirmed(OrderConfirmedEvent{buy.id, Price::fromDouble(4337.10, spec)});

    TickEvent tick;
    tick.bid = Price::fromDouble(4337.99, spec);
    tick.ask = Price::fromDouble(4338.01, spec);  // mid 4338.00, spread 0.02
    engine.onTick(tick);
    const BasketPnlResult pnl = engine.lastBasketPnl();

    // 0.01 lot: $1 per 1.00 move.
    REQUIRE(pnl.grossPnlMoney == Catch::Approx(1.00));       // 4337.00 (requested) -> 4338.00 (mid)
    REQUIRE(pnl.slippageMoney == Catch::Approx(0.10));       // filled 0.10 worse
    REQUIRE(pnl.spreadCostMoney == Catch::Approx(0.01));     // half the 0.02 spread to close
    REQUIRE(pnl.commissionMoney == Catch::Approx(0.07));     // $7/lot round turn * 0.01
    // = what closing right now actually nets: bid 4337.99 - fill 4337.10 = $0.89, minus commission.
    REQUIRE(pnl.netPnlMoney == Catch::Approx(0.89 - 0.07));
}

TEST_CASE("startSide=BuyOnly: one Buy Stop, then strictly one order at a time, alternating") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);  // range 4336.00 - 4337.00
    cfg.startSide = StartSide::BuyOnly;
    StrategyEngine engine(cfg);

    const CommandBatch initial = engine.start();
    REQUIRE(initial.size() == 1);  // no Sell Stop yet
    const auto& buy1 = placeCmd(initial, 0);
    REQUIRE(buy1.side == OrderSide::BuyStop);
    REQUIRE(buy1.price == Price::fromDouble(4337.00, spec));
    REQUIRE(buy1.volume.toDouble(spec) == Catch::Approx(0.01));
    REQUIRE(engine.activeOrderCount() == 1);

    // Buy fills -> the Sell Stop (next lot) goes on at the lower boundary.
    CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{buy1.id, Price::fromDouble(4337.00, spec)});
    REQUIRE(out.size() == 1);
    const auto& sell = placeCmd(out, 0);
    REQUIRE(sell.side == OrderSide::SellStop);
    REQUIRE(sell.price == Price::fromDouble(4336.00, spec));
    REQUIRE(sell.volume.toDouble(spec) == Catch::Approx(0.02));
    REQUIRE(engine.activeOrderCount() == 1);

    // Sell fills -> a Buy Stop back at the top, next lot again.
    out = engine.onOrderConfirmed(OrderConfirmedEvent{sell.id, Price::fromDouble(4336.00, spec)});
    REQUIRE(out.size() == 1);
    REQUIRE(placeCmd(out, 0).side == OrderSide::BuyStop);
    REQUIRE(placeCmd(out, 0).price == Price::fromDouble(4337.00, spec));
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.03));
    REQUIRE(engine.activeOrderCount() == 1);
}

TEST_CASE("startSide=BuyOnly with seeding: one Buy seed above the ask; its fill anchors the range") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);  // width 1.00
    cfg.startSide = StartSide::BuyOnly;
    cfg.seedOffsetPoints = 0.05;
    StrategyEngine engine(cfg);

    TickEvent tick;
    tick.bid = Price::fromDouble(4280.88, spec);
    tick.ask = Price::fromDouble(4280.99, spec);
    engine.onTick(tick);

    const CommandBatch initial = engine.start();
    REQUIRE(initial.size() == 1);
    const auto& seed = placeCmd(initial, 0);
    REQUIRE(seed.side == OrderSide::BuyStop);
    REQUIRE(seed.price == Price::fromDouble(4281.04, spec));  // ask + 0.05
    REQUIRE(seed.volume.toDouble(spec) == Catch::Approx(0.01));
    REQUIRE(engine.seeding());

    // Fill at 4281.04: that becomes the top of the range, the Sell Stop goes
    // one range-width below it, at the next lot -- and nothing is cancelled,
    // since there was no second seed.
    const CommandBatch out = engine.onOrderConfirmed(OrderConfirmedEvent{seed.id, Price::fromDouble(4281.04, spec)});
    REQUIRE(out.size() == 1);
    const auto& sell = placeCmd(out, 0);
    REQUIRE(sell.side == OrderSide::SellStop);
    REQUIRE(sell.price == Price::fromDouble(4280.04, spec));
    REQUIRE(sell.volume.toDouble(spec) == Catch::Approx(0.02));
    REQUIRE_FALSE(engine.seeding());
    REQUIRE(engine.currentRange().upperBound == Price::fromDouble(4281.04, spec));
}

TEST_CASE("stopCycle() abandons the cycle silently and start() can arm a fresh one with unique ids") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));

    const CommandBatch first = engine.start();
    const OrderId firstBuyId = placeCmd(first, 0).id;
    engine.onOrderConfirmed(OrderConfirmedEvent{firstBuyId, Price::fromDouble(4337.00, spec)});
    REQUIRE(engine.activeOrderCount() == 1);

    engine.stopCycle();
    REQUIRE(engine.status() == EngineStatus::Idle);
    REQUIRE(engine.activeOrderCount() == 0);
    REQUIRE(engine.lastBasketPnl().grossPnlMoney == 0.0);

    const CommandBatch second = engine.start();
    REQUIRE(second.size() == 2);
    REQUIRE(engine.status() == EngineStatus::Active);
    // Sequence (and so lot size) restarts, but order ids keep counting up:
    // they're what the broker sees as ClOrdID, so they must never repeat.
    REQUIRE(placeCmd(second, 0).sequenceIndex == 1);
    REQUIRE(placeCmd(second, 0).volume.toDouble(spec) == Catch::Approx(0.01));
    REQUIRE(placeCmd(second, 0).id > placeCmd(first, 1).id);
}

TEST_CASE("reconfigure() changes the settings used by the next cycle") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));

    StrategyConfig tuned = baseConfig(spec);
    tuned.range = RangeConfig{Price::fromDouble(4300.00, spec), Price::fromDouble(4302.00, spec)};
    tuned.lotProgression = LotProgressionConfig{Volume::fromDouble(0.05, spec), Volume::fromDouble(0.05, spec)};
    engine.reconfigure(tuned);

    const CommandBatch out = engine.start();
    REQUIRE(placeCmd(out, 0).price == Price::fromDouble(4302.00, spec));
    REQUIRE(placeCmd(out, 0).volume.toDouble(spec) == Catch::Approx(0.05));
    REQUIRE(placeCmd(out, 1).price == Price::fromDouble(4300.00, spec));
    REQUIRE(placeCmd(out, 1).volume.toDouble(spec) == Catch::Approx(0.10));
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

TEST_CASE("Commission is charged per lot actually traded: $7/lot on 0.01 + 0.02 + 0.03 lots is $0.42") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.costs.commissionPerLotRoundTurn = 7.0;
    StrategyEngine engine(cfg);
    (void)engine.onTick(TickEvent{Price::fromDouble(4336.50, spec), Price::fromDouble(4336.50, spec)});
    const CommandBatch start = engine.start();  // Buy 0.01 @ 4337, Sell 0.02 @ 4336
    const PlaceOrderCommand buy1 = placeCmd(start, 0);
    const PlaceOrderCommand sell2 = placeCmd(start, 1);
    REQUIRE(buy1.volume.toDouble(spec) == Catch::Approx(0.01));
    REQUIRE(sell2.volume.toDouble(spec) == Catch::Approx(0.02));

    (void)engine.onOrderConfirmed(OrderConfirmedEvent{buy1.id, buy1.price});
    const CommandBatch after2 = engine.onOrderConfirmed(OrderConfirmedEvent{sell2.id, sell2.price});
    const PlaceOrderCommand buy3 = placeCmd(after2, 0);
    REQUIRE(buy3.volume.toDouble(spec) == Catch::Approx(0.03));
    (void)engine.onOrderConfirmed(OrderConfirmedEvent{buy3.id, buy3.price});

    (void)engine.onTick(TickEvent{Price::fromDouble(4337.00, spec), Price::fromDouble(4337.00, spec)});
    const BasketPnlResult pnl = engine.lastBasketPnl();
    REQUIRE(pnl.commissionMoney == Catch::Approx(0.07 + 0.14 + 0.21));
    REQUIRE(pnl.spreadCostMoney == Catch::Approx(0.0));  // zero-spread tick
}

TEST_CASE("Spread cost is exactly half the spread per lot, also for an odd number of ticks") {
    const SymbolSpec spec = xauusdSpec();
    StrategyEngine engine(baseConfig(spec));
    (void)engine.onTick(TickEvent{Price::fromDouble(4336.50, spec), Price::fromDouble(4336.50, spec)});
    const CommandBatch start = engine.start();
    const PlaceOrderCommand buy1 = placeCmd(start, 0);  // 0.01 lot @ 4337.00
    (void)engine.onOrderConfirmed(OrderConfirmedEvent{buy1.id, buy1.price});

    // Bid 4338.00 / ask 4338.05: mid 4338.025, 5-tick spread.
    (void)engine.onTick(TickEvent{Price::fromDouble(4338.00, spec), Price::fromDouble(4338.05, spec)});
    const BasketPnlResult pnl = engine.lastBasketPnl();
    REQUIRE(pnl.grossPnlMoney == Catch::Approx(1.025));     // 102.5 ticks x $1 x 0.01 lot
    REQUIRE(pnl.spreadCostMoney == Catch::Approx(0.025));   // 2.5 ticks x $1 x 0.01 lot
    REQUIRE(pnl.netPnlMoney == Catch::Approx(1.00));        // = selling at the bid
}

TEST_CASE("exportState/restoreState: a restored engine carries on the same basket exactly") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.startSide = StartSide::BuyOnly;
    cfg.basketProfitTargetMoney = 1.0;
    StrategyEngine original(cfg);
    (void)original.onTick(TickEvent{Price::fromDouble(4336.50, spec), Price::fromDouble(4336.50, spec)});
    const PlaceOrderCommand buy1 = placeCmd(original.start(), 0);
    const PlaceOrderCommand sell2 = placeCmd(original.onOrderConfirmed(OrderConfirmedEvent{buy1.id, buy1.price}), 0);
    REQUIRE(sell2.side == OrderSide::SellStop);

    const EngineState saved = original.exportState();
    REQUIRE(saved.status == EngineStatus::Active);
    REQUIRE(saved.pendingOrders.size() == 1);
    REQUIRE(saved.buyVolumeSteps == 1);

    // "New process": same config, then the saved state.
    StrategyEngine resumed(cfg);
    resumed.restoreState(saved);
    REQUIRE(resumed.status() == EngineStatus::Active);
    REQUIRE(resumed.activeOrderCount() == 1);
    REQUIRE(resumed.nextSequenceIndex() == original.nextSequenceIndex());

    // Both engines react identically to the same next fill...
    const CommandBatch a = original.onOrderConfirmed(OrderConfirmedEvent{sell2.id, sell2.price});
    const CommandBatch b = resumed.onOrderConfirmed(OrderConfirmedEvent{sell2.id, sell2.price});
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    REQUIRE(placeCmd(a, 0).id == placeCmd(b, 0).id);  // order ids continue, never reused
    REQUIRE(placeCmd(a, 0).price == placeCmd(b, 0).price);
    REQUIRE(placeCmd(a, 0).volume == placeCmd(b, 0).volume);

    // ...and close the basket on the same tick.
    const TickEvent up{Price::fromDouble(4338.20, spec), Price::fromDouble(4338.20, spec)};
    const CommandBatch closeA = original.onTick(up);
    const CommandBatch closeB = resumed.onTick(up);
    REQUIRE(closeA.size() == closeB.size());
    REQUIRE(resumed.status() == original.status());
    REQUIRE(resumed.lastBasketPnl().netPnlMoney == Catch::Approx(original.lastBasketPnl().netPnlMoney));
}

TEST_CASE("restoreState of an idle state only carries the order-id counter forward") {
    const SymbolSpec spec = xauusdSpec();
    EngineState idle;
    idle.nextOrderId = 500;
    StrategyEngine engine(baseConfig(spec));
    engine.restoreState(idle);
    REQUIRE(engine.status() == EngineStatus::Idle);
    REQUIRE(placeCmd(engine.start(), 0).id == 500);
}

TEST_CASE("armNextAfter arms the missing next order of a resumed basket; replaceOrder re-arms a lost one") {
    const SymbolSpec spec = xauusdSpec();
    StrategyConfig cfg = baseConfig(spec);
    cfg.startSide = StartSide::BuyOnly;
    StrategyEngine engine(cfg);

    // A basket rebuilt from the broker: BUY 0.01, SELL 0.02, BUY 0.03 open, nothing pending.
    EngineState s;
    s.status = EngineStatus::Active;
    s.range = cfg.range;
    s.buyVolumeSteps = 4;
    s.buySumEntryPriceTicksVolume = cfg.range.upperBound.ticks * 4;
    s.sellVolumeSteps = 2;
    s.sellSumEntryPriceTicksVolume = cfg.range.lowerBound.ticks * 2;
    s.nextSequenceIndex = 4;
    s.nextOrderId = 10;
    engine.restoreState(s);

    const CommandBatch next = engine.armNextAfter(OrderSide::BuyStop);
    REQUIRE(next.size() == 1);
    const PlaceOrderCommand sell4 = placeCmd(next, 0);
    REQUIRE(sell4.side == OrderSide::SellStop);
    REQUIRE(sell4.price == cfg.range.lowerBound);
    REQUIRE(sell4.volume.toDouble(spec) == Catch::Approx(0.04));
    REQUIRE(sell4.id == 10);
    REQUIRE(engine.armNextAfter(OrderSide::BuyStop).empty());  // already pending: never twice

    const CommandBatch again = engine.replaceOrder(sell4.id);
    REQUIRE(again.size() == 1);
    const PlaceOrderCommand replacement = placeCmd(again, 0);
    REQUIRE(replacement.id != sell4.id);
    REQUIRE(replacement.side == OrderSide::SellStop);
    REQUIRE(replacement.price == sell4.price);
    REQUIRE(replacement.volume == sell4.volume);
    REQUIRE(replacement.sequenceIndex == sell4.sequenceIndex);
    REQUIRE(engine.activeOrderCount() == 1);

    // Its fill carries the sequence on as usual: next is a Buy Stop 0.05.
    const CommandBatch after = engine.onOrderConfirmed(OrderConfirmedEvent{replacement.id, replacement.price});
    REQUIRE(placeCmd(after, 0).side == OrderSide::BuyStop);
    REQUIRE(placeCmd(after, 0).volume.toDouble(spec) == Catch::Approx(0.05));
}
