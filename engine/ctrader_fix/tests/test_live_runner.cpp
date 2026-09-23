#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::core;
using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kTradeConn = 0;  // LiveRunner connects Trade first, then Quote (live_runner.cpp)
constexpr std::size_t kQuoteConn = 1;

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

LiveConfig testConfig(const MockFixServer& server) {
    LiveConfig cfg;
    cfg.connection.quote = FixEndpoint{"127.0.0.1", server.port()};
    cfg.connection.trade = FixEndpoint{"127.0.0.1", server.port()};
    cfg.connection.environment = "demo";
    cfg.connection.brokerUid = "testbroker";
    cfg.connection.traderLogin = "12345";
    cfg.connection.password = "pass";
    cfg.connection.heartBtIntSeconds = 1;
    cfg.connection.symbolName = "XAUUSD";

    const SymbolSpec spec = xauusdSpec();
    cfg.strategy.symbol = spec;
    cfg.strategy.range = RangeConfig{Price::fromDouble(4336.00, spec), Price::fromDouble(4337.00, spec)};
    cfg.strategy.lotProgression =
        LotProgressionConfig{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};
    cfg.strategy.riskLimits.maxOrders = 100;
    cfg.strategy.riskLimits.maxLot = Volume::fromDouble(10.0, spec);
    cfg.strategy.basketProfitTargetMoney = 0.0;  // disabled: keep this test focused on order flow
    return cfg;
}

FixMessage serverHeaderMessage(const std::string& msgType, int seqNum) {
    FixMessage msg(msgType);
    msg.set(tag::SenderCompID, std::string("CSERVER"));
    msg.set(tag::TargetCompID, std::string("demo.testbroker.12345"));
    msg.set(tag::MsgSeqNum, static_cast<int64_t>(seqNum));
    msg.set(tag::SendingTime, nowUtcTimestamp());
    return msg;
}

FixMessage buildSecurityListResponse() {
    FixMessage msg = serverHeaderMessage(msgtype::SecurityList, 2);
    msg.set(tag::SecurityReqID, std::string("sec-1"));
    msg.set(tag::SecurityResponseID, std::string("secresp-1"));
    msg.set(tag::SecurityRequestResult, int64_t{0});
    msg.set(tag::NoRelatedSym, int64_t{2});
    msg.set(tag::Symbol, std::string("1"));
    msg.set(tag::SymbolName, std::string("XAUUSD"));
    msg.set(tag::SymbolDigits, int64_t{2});
    msg.set(tag::Symbol, std::string("2"));
    msg.set(tag::SymbolName, std::string("EURUSD"));
    msg.set(tag::SymbolDigits, int64_t{5});
    return msg;
}

}  // namespace

TEST_CASE("LiveRunner resolves the symbol, arms both boundary orders, and re-arms on a fill") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    // run() blocks until stop() is called; exceptions are captured rather
    // than left to cross the thread boundary uncaught (which would call
    // std::terminate) and are surfaced via REQUIRE below, on the main
    // thread — Catch2's assertion macros are not safe to invoke off it.
    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    // LiveRunner connects and logs on to both sessions before doing
    // anything else (live_runner.cpp: tradeSession_->start() then
    // quoteSession_->start()), so both Logons must be acknowledged before
    // it will proceed to symbol resolution.
    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    // --- Symbol resolution ---
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    // Quote subscription happens before the engine (and so the initial
    // orders) is constructed -- see live_runner.cpp's run(): that
    // ordering is what lets auto-range-centering observe a live tick
    // first when enabled. autoRangeWidthPoints is 0 in this test's
    // config, so no tick is required before the engine starts; the
    // ordering still holds regardless.
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // --- The initial two boundary orders ---
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order1.getString(tag::Symbol) == "1");
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getInt(tag::OrdType) == 3);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    // OrderQty is lot hundredths on the wire (0.01 lot -> 1.0), not raw
    // lots -- see docs/ctrader-fix.md "Orders".
    REQUIRE(order1.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(order2.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    // --- A harmless in-range tick ---
    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4336.50"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4336.52"));
    server.sendToClient(kQuoteConn, snapshot);

    // --- Broker fills the first (Buy Stop) order. Its opposite (order2,
    // Sell Stop) is already pending, so nothing new should be armed yet —
    // confirmed via snapshot() rather than waiting for a NewOrderSingle
    // that (correctly) never arrives. ---
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);  // only order2 remains pending

    // --- The second (Sell Stop) order now fills. Its opposite (Buy) has
    // nothing pending, so a fresh Buy Stop (order3) is armed. ---
    FixMessage fill2 = serverHeaderMessage(msgtype::ExecutionReport, 4);
    fill2.set(tag::OrderID, std::string("9002"));
    fill2.set(tag::ClOrdID, order2ClOrdId);
    fill2.set(tag::ExecType, std::string("F"));
    fill2.set(tag::OrdStatus, std::string("2"));
    fill2.set(tag::AvgPx, std::string("4336.00"));
    fill2.set(tag::LastQty, std::string("0.02"));
    fill2.set(tag::CumQty, std::string("0.02"));
    fill2.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill2);

    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order3.getInt(tag::Side) == 1);
    REQUIRE(order3.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(order3.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(order3.getString(tag::ClOrdID) != order1ClOrdId);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner flattens the net position with a market order on basket close, "
          "and only starts the next cycle once that fill is confirmed") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;  // low bar, cleared by the rally below
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // --- Cycle 1's initial two boundary orders ---
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);

    // --- Buy Stop fills; only a long 0.01-lot leg is open. ---
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    // --- Price rallies well past the profit target -> basket closes. The
    // engine cancels order2 (OrderCancelRequest, not a NewOrderSingle) and
    // emits CloseBasketCommand; LiveRunner must respond with an actual
    // Market order (OrdType=1) to flatten the 0.01-lot long, not just log
    // it and move on. ---
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);  // Market, not Stop
    REQUIRE(closeOrder.getInt(tag::Side) == 2);      // net long -> flatten with a Sell
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    const std::string closeClOrdId = closeOrder.getString(tag::ClOrdID);

    // The basket is closed and the range is flat at the broker, but the
    // next cycle must NOT have started yet: no new boundary order should
    // exist until the closing trade itself is confirmed filled.
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Completed) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    // --- Now confirm the closing trade's fill. Only this unblocks cycle 2. ---
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeClOrdId);
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4342.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);  // Stop -- back to normal strategy entries
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);
    REQUIRE(runner.snapshot().activeOrderCount == 2);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner closes every filled leg individually on basket close, "
          "not just a single net-sized order") {
    // Reproduces a hedging-account scenario: several fills whose *net*
    // exposure alone would not correctly describe what needs closing at
    // the broker (this demo account keeps every fill as its own position
    // rather than merging same-symbol exposure). Three legs end up open
    // (Buy .01, Sell .02, Buy .03) before the basket closes -- LiveRunner
    // must send three separate, individually-sized closing Market orders,
    // not one order for the net (long .02).
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 0.01;  // low bar: closes as soon as onTick sees any real profit
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell .02 @ 4336.00
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);

    auto sendFill = [&](int seqNum, const std::string& clOrdId, const std::string& px, const std::string& qty) {
        FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, seqNum);
        fill.set(tag::OrderID, std::to_string(9000 + seqNum));
        fill.set(tag::ClOrdID, clOrdId);
        fill.set(tag::ExecType, std::string("F"));
        fill.set(tag::OrdStatus, std::string("2"));
        fill.set(tag::AvgPx, px);
        fill.set(tag::LastQty, qty);
        fill.set(tag::CumQty, qty);
        fill.set(tag::LeavesQty, std::string("0"));
        server.sendToClient(kTradeConn, fill);
    };

    // Leg 1: order1 (Buy .01) fills. Opposite (order2, Sell) already
    // pending, so nothing new is armed yet.
    sendFill(3, order1ClOrdId, "4337.00", "0.01");
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().activeOrderCount == 1);

    // Leg 2: order2 (Sell .02) fills -> a fresh Buy Stop (order3) is armed.
    sendFill(4, order2ClOrdId, "4336.00", "0.02");
    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Buy .03 @ 4337.00
    const std::string order3ClOrdId = order3.getString(tag::ClOrdID);

    // Leg 3: order3 (Buy .03) fills -> a fresh Sell Stop (order4) is armed.
    sendFill(5, order3ClOrdId, "4337.00", "0.03");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell .04 @ 4336.00 -- will be cancelled

    // Three legs now open: Buy .01, Sell .02, Buy .03 (net long .02). A
    // rally clears the profit target -> basket closes, order4 is
    // cancelled, and all three legs must be flattened individually.
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 6);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage close1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close1.getInt(tag::OrdType) == 1);
    REQUIRE(close1.getInt(tag::Side) == 2);  // closes the Buy .01 leg
    REQUIRE(close1.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    const FixMessage close2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close2.getInt(tag::OrdType) == 1);
    REQUIRE(close2.getInt(tag::Side) == 1);  // closes the Sell .02 leg
    REQUIRE(close2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    const FixMessage close3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close3.getInt(tag::OrdType) == 1);
    REQUIRE(close3.getInt(tag::Side) == 2);  // closes the Buy .03 leg
    REQUIRE(close3.getDouble(tag::OrderQty) == Catch::Approx(3.0));

    // Not one of these three closing orders has been confirmed filled yet
    // -- the next cycle must still be blocked.
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    sendFill(7, close1.getString(tag::ClOrdID), "4342.00", "0.01");
    sendFill(8, close2.getString(tag::ClOrdID), "4342.00", "0.02");
    // Two of three confirmed: still blocked.
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    sendFill(9, close3.getString(tag::ClOrdID), "4342.00", "0.03");

    // All three legs confirmed flat -> the next cycle starts.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner waits out connection.basketCooldownSeconds after a basket "
          "is fully flat before starting the next cycle") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;
    cfg.connection.basketCooldownSeconds = 1;  // seconds -- short so the test stays fast
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // order2

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().activeOrderCount == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4342.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    // The basket is fully flat now (the only leg's closing trade just
    // confirmed), but the cooldown hasn't elapsed yet -- no new order
    // should appear within a window well short of the 1s cooldown.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 300ms), std::runtime_error);
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    // A quote tick arriving mid-cooldown must not bypass it either.
    FixMessage midCooldownTick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 5);
    midCooldownTick.set(tag::Symbol, std::string("1"));
    midCooldownTick.set(tag::NoMDEntries, int64_t{2});
    midCooldownTick.set(tag::MDEntryType, std::string("0"));
    midCooldownTick.set(tag::MDEntryPx, std::string("4342.00"));
    midCooldownTick.set(tag::MDEntryType, std::string("1"));
    midCooldownTick.set(tag::MDEntryPx, std::string("4342.02"));
    server.sendToClient(kQuoteConn, midCooldownTick);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 300ms), std::runtime_error);
    REQUIRE(runner.snapshot().status == EngineStatus::Completed);

    // Once the cooldown has elapsed, the next tick lets the next cycle start.
    std::this_thread::sleep_for(600ms);  // 300+300+600 > the 1s cooldown
    FixMessage afterCooldownTick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 6);
    afterCooldownTick.set(tag::Symbol, std::string("1"));
    afterCooldownTick.set(tag::NoMDEntries, int64_t{2});
    afterCooldownTick.set(tag::MDEntryType, std::string("0"));
    afterCooldownTick.set(tag::MDEntryPx, std::string("4342.00"));
    afterCooldownTick.set(tag::MDEntryType, std::string("1"));
    afterCooldownTick.set(tag::MDEntryPx, std::string("4342.02"));
    server.sendToClient(kQuoteConn, afterCooldownTick);

    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4337.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner exposes snapshot() and routes status lines through setLogHandler") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::mutex logMutex;
    std::vector<std::string> logLines;
    // Must be set before run() -- see setLogHandler's doc comment.
    runner.setLogHandler([&](const std::string& line) {
        std::scoped_lock lock(logMutex);
        logLines.push_back(line);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    // Quote subscription happens before the engine (and initial orders)
    // is constructed -- see the other TEST_CASE's comment on this.
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // seq1
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // seq2

    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.activeOrderCount == 2);
    REQUIRE(snap.symbolName == "XAUUSD");
    REQUIRE(snap.symbolId == "1");
    REQUIRE(snap.rangeLower == Catch::Approx(4336.00));
    REQUIRE(snap.rangeUpper == Catch::Approx(4337.00));

    {
        std::scoped_lock lock(logMutex);
        const bool sawLogonLine =
            std::any_of(logLines.begin(), logLines.end(),
                        [](const std::string& l) { return l.find("logged on") != std::string::npos; });
        REQUIRE(sawLogonLine);
    }

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner auto-centers the range on the first live quote when autoRangeWidthPoints is set") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoRangeWidthPoints = 0.20;
    // The configured static range is deliberately far from the tick this
    // test sends, so passing only proves auto-centering actually
    // overrode it rather than coincidentally matching.
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));

    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());

    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4292.40"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4292.50"));
    server.sendToClient(kQuoteConn, snapshot);

    // Mid = 4292.45; a 0.20-wide range centered there is [4292.35, 4292.55].
    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order1.getInt(tag::Side) == 1);
    REQUIRE(order1.getDouble(tag::StopPx) == Catch::Approx(4292.55));

    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    REQUIRE(order2.getInt(tag::Side) == 2);
    REQUIRE(order2.getDouble(tag::StopPx) == Catch::Approx(4292.35));

    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.rangeLower == Catch::Approx(4292.35));
    REQUIRE(snap.rangeUpper == Catch::Approx(4292.55));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}
