#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "live_test_helpers.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::core;
using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;


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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
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
    fill2.set(tag::PosMaintRptID, "P" + std::string(order2ClOrdId));
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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
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
    // Aimed at that exact position -- without PosMaintRptID a hedging
    // account opens a new position instead of closing this one.
    REQUIRE(closeOrder.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);
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

    // Cycle 2 re-arms the same fixed range (no auto-centering here). The
    // market (4342) is already above its Buy Stop at 4337, which the broker
    // would refuse -- so that one waits for the price, and only the Sell
    // Stop goes out.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);  // Stop -- back to normal strategy entries
    REQUIRE(cycle2Order1.getInt(tag::Side) == 2);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);
    REQUIRE(runner.snapshot().activeOrderCount == 2);
    const auto cycle2 = runner.snapshot().pendingOrders;
    REQUIRE(cycle2.size() == 2);
    REQUIRE(std::count_if(cycle2.begin(), cycle2.end(), [](const PendingOrderInfo& o) { return o.waiting; }) == 1);

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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
        fill.set(tag::PosMaintRptID, "P" + std::string(clOrdId));
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
    REQUIRE(close1.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);

    const FixMessage close2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close2.getInt(tag::OrdType) == 1);
    REQUIRE(close2.getInt(tag::Side) == 1);  // closes the Sell .02 leg
    REQUIRE(close2.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(close2.getString(tag::PosMaintRptID) == "P" + order2ClOrdId);

    const FixMessage close3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close3.getInt(tag::OrdType) == 1);
    REQUIRE(close3.getInt(tag::Side) == 2);  // closes the Buy .03 leg
    REQUIRE(close3.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(close3.getString(tag::PosMaintRptID) == "P" + order3ClOrdId);

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
    // The fixed range's Buy Stop (4337) is below the market (4342) here, so
    // it waits for the price; the Sell Stop is what goes out first.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Active) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner closes a cancelled order's fill immediately if it wins the race against its own cancel") {
    // Reproduces the reported "bot opened 2 positions when the range
    // started" bug: an order LiveRunner has already asked the broker to
    // cancel can still fill anyway (a real timing race -- the cancel
    // request and the fill can cross in flight). The surplus leg must be
    // closed right away, not silently left open until the next basket
    // close.
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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
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

    // Rally clears the profit target -> basket closes: order1's leg gets
    // an individual closing Market order, and order2 (still pending) gets
    // an OrderCancelRequest.
    FixMessage rally = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 4);
    rally.set(tag::Symbol, std::string("1"));
    rally.set(tag::NoMDEntries, int64_t{2});
    rally.set(tag::MDEntryType, std::string("0"));
    rally.set(tag::MDEntryPx, std::string("4341.99"));
    rally.set(tag::MDEntryType, std::string("1"));
    rally.set(tag::MDEntryPx, std::string("4342.01"));
    server.sendToClient(kQuoteConn, rally);

    const FixMessage closeOrder1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder1.getInt(tag::OrdType) == 1);
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);  // order2's cancel

    // The race: order2 fills anyway, despite the cancel already in flight.
    FixMessage fill2 = serverHeaderMessage(msgtype::ExecutionReport, 5);
    fill2.set(tag::OrderID, std::string("9002"));
    fill2.set(tag::ClOrdID, order2ClOrdId);
    fill2.set(tag::PosMaintRptID, "P" + std::string(order2ClOrdId));
    fill2.set(tag::ExecType, std::string("F"));
    fill2.set(tag::OrdStatus, std::string("2"));
    fill2.set(tag::AvgPx, std::string("4336.00"));
    fill2.set(tag::LastQty, std::string("0.02"));
    fill2.set(tag::CumQty, std::string("0.02"));
    fill2.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill2);

    // A second, separate closing Market order for the surplus leg must
    // appear -- not silence, not a wait for the next basket cycle.
    const FixMessage closeOrder2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder2.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder2.getInt(tag::Side) == 1);  // closes the Sell .02 leg
    REQUIRE(closeOrder2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::closePosition and cancelOrder act on a single row without disturbing the rest") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const OrderId order1Id = static_cast<OrderId>(std::stoull(order1ClOrdId));
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell .02 @ 4336.00
    const std::string order2ClOrdId = order2.getString(tag::ClOrdID);
    const OrderId order2Id = static_cast<OrderId>(std::stoull(order2ClOrdId));

    // Fill order1 -> one open position, order2 stays a pending order.
    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }

    const LiveSnapshot beforeSnap = runner.snapshot();
    REQUIRE(beforeSnap.positions.size() == 1);
    REQUIRE(beforeSnap.positions[0].id == order1Id);
    REQUIRE(beforeSnap.positions[0].lots == Catch::Approx(0.01));
    REQUIRE(beforeSnap.pendingOrders.size() == 1);
    REQUIRE(beforeSnap.pendingOrders[0].id == order2Id);
    REQUIRE(beforeSnap.status == EngineStatus::Active);  // engine untouched by per-item actions

    runner.closePosition(order1Id);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getInt(tag::Side) == 2);
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    runner.cancelOrder(order2Id);
    const FixMessage cancelReq = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancelReq.getString(tag::OrigClOrdID) == order2ClOrdId);

    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.empty() && s.pendingOrders.empty()) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot afterSnap = runner.snapshot();
    REQUIRE(afterSnap.positions.empty());
    REQUIRE(afterSnap.pendingOrders.empty());
    // Per-item actions leave the engine running -- unlike closeAllPositions()/
    // cancelAllPendingOrders(), which reset it to Idle.
    REQUIRE(afterSnap.status == EngineStatus::Active);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner snapshot shows every cost and a live net P/L; booked P/L comes from the real exit fill") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.costs.commissionPerLotRoundTurn = 7.0;  // $7 per lot round turn
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy Stop 0.01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Sell Stop, stays pending

    // Fills 0.10 worse than requested.
    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1ClOrdId);
    fill.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.10"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4337.99"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4338.01"));  // mid 4338.00
    server.sendToClient(kQuoteConn, tick);

    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.size() == 1 && s.bid.has_value()) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 1);
    const PositionInfo& pos = snap.positions[0];
    REQUIRE(pos.requestedPrice == Catch::Approx(4337.00));
    REQUIRE(pos.entryPrice == Catch::Approx(4337.10));
    REQUIRE(pos.pnl.gross == Catch::Approx(1.00));       // 4337.00 -> 4338.00, 0.01 lot
    REQUIRE(pos.pnl.slippage == Catch::Approx(0.10));
    REQUIRE(pos.pnl.spread == Catch::Approx(0.01));      // mid -> bid
    REQUIRE(pos.pnl.commission == Catch::Approx(0.07));
    REQUIRE(pos.pnl.net == Catch::Approx(0.82));         // bid 4337.99 - fill 4337.10 = 0.89, minus 0.07
    REQUIRE(snap.live.net == Catch::Approx(0.82));
    REQUIRE(snap.live.costs() == Catch::Approx(0.18));

    // Close it; the broker fills the close at 4337.95 (a little worse than the bid).
    runner.closePosition(pos.id);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4337.95"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    for (int i = 0; i < 50; ++i) {
        if (!runner.snapshot().closingInProgress) break;
        std::this_thread::sleep_for(50ms);
    }
    const LiveSnapshot after = runner.snapshot();
    REQUIRE(after.bookedGross == Catch::Approx(0.85));       // real fills: 4337.10 -> 4337.95
    REQUIRE(after.bookedCommission == Catch::Approx(0.07));
    REQUIRE(after.realizedPnl == Catch::Approx(0.78));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner records each closed basket with its position tree, and measures its own speed") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.strategy.costs.commissionPerLotRoundTurn = 7.0;
    LiveRunner runner(cfg);

    std::mutex savedMutex;
    std::vector<BasketRecord> saved;
    runner.setBasketHandler([&](const BasketRecord& b) {
        std::scoped_lock lock(savedMutex);
        saved.push_back(b);
    });

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy Stop 0.01 @ 4337.00
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);                      // Sell Stop, stays pending

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1.getString(tag::ClOrdID));
    fill.set(tag::PosMaintRptID, std::string("P77"));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.00"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4338.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4338.02"));
    server.sendToClient(kQuoteConn, tick);
    for (int i = 0; i < 50; ++i) {
        const LiveSnapshot s = runner.snapshot();
        if (s.positions.size() == 1 && s.ticksProcessed > 0) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().currentBasket.has_value());
    REQUIRE(runner.snapshot().currentBasket->legs.size() == 1);

    runner.closeRange();
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::PosMaintRptID, std::string("P77"));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4338.00"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);

    for (int i = 0; i < 50; ++i) {
        {
            std::scoped_lock lock(savedMutex);
            if (!saved.empty()) break;
        }
        std::this_thread::sleep_for(50ms);
    }
    {
        std::scoped_lock lock(savedMutex);
        REQUIRE(saved.size() == 1);
        const BasketRecord& b = saved[0];
        REQUIRE(b.number == 1);
        REQUIRE(b.closeReason == "manual: close range");
        REQUIRE(b.legs.size() == 1);
        REQUIRE(b.legs[0].buy);
        REQUIRE(b.legs[0].positionId == "P77");
        REQUIRE(b.legs[0].entryPrice == Catch::Approx(4337.00));
        REQUIRE(b.legs[0].exitPrice == Catch::Approx(4338.00));
        REQUIRE(b.legs[0].gross == Catch::Approx(1.00));
        REQUIRE(b.legs[0].commission == Catch::Approx(0.07));
        REQUIRE(b.net == Catch::Approx(0.93));
        REQUIRE_FALSE(b.closedAt.empty());
    }

    const LiveSnapshot after = runner.snapshot();
    REQUIRE(after.baskets.size() == 1);
    REQUIRE_FALSE(after.currentBasket.has_value());
    REQUIRE(after.tickDecisionUs.samples >= 1);
    REQUIRE(after.brokerRoundTripMs.samples >= 1);  // order sent -> its fill report
    REQUIRE_FALSE(after.sessionStartedAt.empty());

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner never sends a close for a position whose id the broker didn't give it") {
    // On a hedging account an opposite order without PosMaintRptID opens a
    // *new* position -- so a leg with no known position id must not be
    // "closed" blindly.
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);  // deliberately no tag 721
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1.getString(tag::ClOrdID));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.00"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    const OrderId handle = runner.snapshot().positions.at(0).id;

    runner.closePosition(handle);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 400ms), std::runtime_error);
    const LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 1);  // still listed -- it's still open at the broker
    REQUIRE(snap.closeFailed);            // and flagged, so no new range opens on top of it

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner syncs open positions from the broker on connect and can close them by position id") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.syncPositionsOnConnect = true;
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    // Two XAUUSD positions left over from an earlier run, plus one on another symbol.
    const FixMessage request = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 3s);
    const std::string posReqId = request.getString(tag::PosReqID);
    auto report = [&](int seq, const char* positionId, const char* symbol, const char* longQty, const char* shortQty,
                      const char* price) {
        FixMessage r = serverHeaderMessage(msgtype::PositionReport, seq);
        r.set(tag::PosReqID, posReqId);
        r.set(tag::PosMaintRptID, std::string(positionId));
        r.set(tag::TotalNumPosReports, int64_t{3});
        r.set(tag::PosReqResult, std::string("0"));
        r.set(tag::Symbol, std::string(symbol));
        r.set(tag::NoPositions, int64_t{1});
        r.set(tag::LongQty, std::string(longQty));
        r.set(tag::ShortQty, std::string(shortQty));
        r.set(tag::SettlPrice, std::string(price));
        server.sendToClient(kTradeConn, r);
    };
    report(3, "P100", "1", "3", "0", "4280.00");  // BUY 0.03 lot
    report(4, "P200", "1", "0", "2", "4281.00");  // SELL 0.02 lot
    report(5, "P900", "2", "100000", "0", "1.10");  // EURUSD -- not ours, ignored
    // ...and no working orders.
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 3s);
    FixMessage noOrders = serverHeaderMessage(msgtype::BusinessMessageReject, 6);
    noOrders.set(tag::RefMsgType, std::string("AF"));
    noOrders.set(tag::BusinessRejectRefID, ordReq.getString(tag::MassStatusReqID));
    server.sendToClient(kTradeConn, noOrders);
    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4280.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4280.10"));
    server.sendToClient(kQuoteConn, tick);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().connected) break;
        std::this_thread::sleep_for(50ms);
    }
    LiveSnapshot snap = runner.snapshot();
    REQUIRE(snap.positions.size() == 2);
    REQUIRE(snap.positions[0].adopted);
    REQUIRE(snap.positions[0].positionId == "P100");
    REQUIRE(snap.positions[0].side == OrderSide::BuyStop);
    REQUIRE(snap.positions[0].lots == Catch::Approx(0.03));
    REQUIRE(snap.positions[0].entryPrice == Catch::Approx(4280.00));
    REQUIRE(snap.positions[1].positionId == "P200");
    REQUIRE(snap.positions[1].side == OrderSide::SellStop);
    // autoContinue is on, but the account isn't flat -- no range opens.
    REQUIRE(snap.status == EngineStatus::Idle);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 400ms), std::runtime_error);

    runner.closeAllPositions();
    const FixMessage c1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    const FixMessage c2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(c1.getInt(tag::OrdType) == 1);
    REQUIRE(c1.getString(tag::PosMaintRptID) == "P100");
    REQUIRE(c1.getInt(tag::Side) == 2);
    REQUIRE(c1.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(c2.getString(tag::PosMaintRptID) == "P200");
    REQUIRE(c2.getInt(tag::Side) == 1);
    REQUIRE(c2.getDouble(tag::OrderQty) == Catch::Approx(2.0));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::openRange/closeRange: one range at a time, never on top of an unfinished one") {
    MockFixServer server;
    LiveConfig cfg = testConfig(server);
    cfg.connection.autoContinue = false;  // nothing opens until the operator asks
    LiveRunner runner(cfg);

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().connected) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().connected);
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);  // autoContinue off: no orders yet

    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, std::string("4300.00"));
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, std::string("4300.10"));
    server.sendToClient(kQuoteConn, tick);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().bid.has_value()) break;
        std::this_thread::sleep_for(50ms);
    }

    // Tune before opening: $2 range, 0.02 starting lot.
    LiveSettings s = runner.settings();
    s.rangeWidth = 2.0;
    s.initialLot = 0.02;
    s.lotIncrement = 0.01;
    runner.applySettings(s);

    REQUIRE(runner.openRange());
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(buy.getInt(tag::Side) == 1);
    REQUIRE(buy.getDouble(tag::StopPx) == Catch::Approx(4301.05));  // mid 4300.05 + 1.00
    REQUIRE(buy.getDouble(tag::OrderQty) == Catch::Approx(2.0));     // 0.02 lot
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(sell.getInt(tag::Side) == 2);
    REQUIRE(sell.getDouble(tag::StopPx) == Catch::Approx(4299.05));
    REQUIRE(sell.getDouble(tag::OrderQty) == Catch::Approx(3.0));

    REQUIRE_FALSE(runner.openRange());  // one already running

    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, buy.getString(tag::ClOrdID));
    fill.set(tag::PosMaintRptID, "P" + std::string(buy.getString(tag::ClOrdID)));
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4301.05"));
    fill.set(tag::LastQty, std::string("0.02"));
    fill.set(tag::CumQty, std::string("0.02"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().positions.size() == 1);

    // Close range: cancel the pending Sell, close the open Buy, go Idle.
    runner.closeRange();
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == sell.getString(tag::ClOrdID));
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getInt(tag::Side) == 2);
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().closingInProgress);

    // The closing trade hasn't confirmed yet -- the account isn't flat, so
    // no new range may open.
    REQUIRE_FALSE(runner.openRange());

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4300.00"));
    closeFill.set(tag::LastQty, std::string("0.02"));
    closeFill.set(tag::CumQty, std::string("0.02"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);
    for (int i = 0; i < 50; ++i) {
        if (!runner.snapshot().closingInProgress) break;
        std::this_thread::sleep_for(50ms);
    }

    // Flat again, and closeRange() left the engine Idle with auto-continue
    // off -- nothing opened by itself in the meantime.
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.openRange());
    const FixMessage reopened = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(reopened.getInt(tag::OrdType) == 3);
    // Order ids keep counting up across ranges -- the broker never sees a
    // ClOrdID reused.
    REQUIRE(std::stoull(reopened.getString(tag::ClOrdID)) > std::stoull(sell.getString(tag::ClOrdID)));

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("saveLiveSettings rewrites only the tunable fields and round-trips through loadLiveConfig") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_settings.local.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "connection": {
    "quote": { "host": "q.example", "port": "5201" },
    "trade": { "host": "t.example", "port": "5202" },
    "brokerUid": "broker", "traderLogin": "123", "password": "secret",
    "symbolName": "XAUUSD", "symbolId": "41"
  },
  "strategy": {
    "symbol": { "name": "XAUUSD", "digits": 2, "tickSize": 0.01, "tickValue": 1.0, "contractSize": 100.0,
                "volumeStep": 0.01, "volumeMin": 0.01, "volumeMax": 100.0 },
    "range": { "lower": 4292.35, "upper": 4292.55 },
    "lotProgression": { "initialLot": 0.01, "lotIncrement": 0.01 },
    "basketProfitTargetMoney": 5.0
  }
})";
    }

    LiveSettings s;
    s.rangeWidth = 3.0;
    s.seedOffset = 0.10;
    s.initialLot = 0.02;
    s.lotIncrement = 0.02;
    s.profitTarget = 1.5;
    s.cooldownSeconds = 45;
    s.maxOrders = 12;
    s.maxLot = 1.0;
    s.maxBasketLoss = 200.0;
    s.autoContinue = false;
    s.startSide = StartSide::BuyOnly;
    s.commissionPerLot = 7.0;
    saveLiveSettings(path, s);

    const LiveConfig reloaded = loadLiveConfig(path);
    REQUIRE(reloaded.connection.password == "secret");  // credentials untouched
    REQUIRE(reloaded.connection.symbolId == "41");
    const LiveSettings back = settingsFromConfig(reloaded);
    REQUIRE(back.rangeWidth == Catch::Approx(3.0));
    REQUIRE(back.seedOffset == Catch::Approx(0.10));
    REQUIRE(back.initialLot == Catch::Approx(0.02));
    REQUIRE(back.lotIncrement == Catch::Approx(0.02));
    REQUIRE(back.profitTarget == Catch::Approx(1.5));
    REQUIRE(back.cooldownSeconds == 45);
    REQUIRE(back.maxOrders == 12);
    REQUIRE(back.maxLot == Catch::Approx(1.0));
    REQUIRE(back.maxBasketLoss == Catch::Approx(200.0));
    REQUIRE_FALSE(back.autoContinue);
    REQUIRE(back.startSide == StartSide::BuyOnly);
    REQUIRE(back.commissionPerLot == Catch::Approx(7.0));

    std::filesystem::remove(path);
}

TEST_CASE("LiveRunner::closeAllPositions closes every open leg and idles the engine") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // Buy .01 @ 4337.00
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);  // order2, left pending

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
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

    // One open leg (Buy .01, from order1) and one still-pending order
    // (order2, Sell) at this point. closeAllPositions() must only touch
    // the open leg -- pending orders are cancelAllPendingOrders()'s job.
    runner.closeAllPositions();

    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);  // Market
    REQUIRE(closeOrder.getInt(tag::Side) == 2);      // opposite of the Buy leg
    REQUIRE(closeOrder.getDouble(tag::OrderQty) == Catch::Approx(1.0));

    // The engine is now a fresh, Idle instance -- it will not act on its
    // own until the operator explicitly restarts the bot.
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Idle) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::flattenAndWait closes everything and returns only once the broker confirms it") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1ClOrdId = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);

    FixMessage fill1 = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill1.set(tag::OrderID, std::string("9001"));
    fill1.set(tag::ClOrdID, order1ClOrdId);
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
    fill1.set(tag::ExecType, std::string("F"));
    fill1.set(tag::OrdStatus, std::string("2"));
    fill1.set(tag::AvgPx, std::string("4337.00"));
    fill1.set(tag::LastQty, std::string("0.01"));
    fill1.set(tag::CumQty, std::string("0.01"));
    fill1.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill1);
    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().positions.size() == 1) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().positions.size() == 1);

    // The broker never confirms the close in time: the guard must say so
    // rather than claim the account is flat.
    REQUIRE_FALSE(runner.flattenAndWait(300ms));
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == order2.getString(tag::ClOrdID));
    const FixMessage closeOrder = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(closeOrder.getInt(tag::OrdType) == 1);
    REQUIRE(closeOrder.getString(tag::PosMaintRptID) == "P" + order1ClOrdId);
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);

    // Called again while the close is still in flight: no second close is
    // sent, and it returns true as soon as the fill arrives.
    std::atomic<bool> finished{false};
    std::atomic<bool> flat{false};
    std::thread guard([&] {
        flat = runner.flattenAndWait(5s);
        finished = true;
    });
    std::this_thread::sleep_for(200ms);
    REQUIRE_FALSE(finished.load());

    FixMessage closeFill = serverHeaderMessage(msgtype::ExecutionReport, 4);
    closeFill.set(tag::OrderID, std::string("9002"));
    closeFill.set(tag::ClOrdID, closeOrder.getString(tag::ClOrdID));
    closeFill.set(tag::ExecType, std::string("F"));
    closeFill.set(tag::OrdStatus, std::string("2"));
    closeFill.set(tag::AvgPx, std::string("4336.90"));
    closeFill.set(tag::LastQty, std::string("0.01"));
    closeFill.set(tag::CumQty, std::string("0.01"));
    closeFill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, closeFill);
    guard.join();
    REQUIRE(flat.load());
    REQUIRE(runner.snapshot().positions.empty());
    REQUIRE(runner.snapshot().pendingOrders.empty());
    REQUIRE(runner.snapshot().baskets.size() == 1);
    REQUIRE(runner.snapshot().baskets.back().closeReason == "guard: app closing");

    REQUIRE(runner.flattenAndWait(1s));  // already flat: immediate

    runner.stop();
    runnerThread.join();
    if (runnerException) std::rethrow_exception(runnerException);
}

TEST_CASE("LiveRunner::cancelAllPendingOrders cancels every pending order and idles the engine") {
    MockFixServer server;
    LiveRunner runner(testConfig(server));

    std::exception_ptr runnerException;
    std::thread runnerThread([&] {
        try {
            runner.run();
        } catch (...) {
            runnerException = std::current_exception();
        }
    });
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    const FixMessage order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order1Id = order1.getString(tag::ClOrdID);
    const FixMessage order2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn);
    const std::string order2Id = order2.getString(tag::ClOrdID);

    runner.cancelAllPendingOrders();

    const FixMessage cancel1 = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage cancel2 = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const std::vector<std::string> cancelledIds = {cancel1.getString(tag::OrigClOrdID),
                                                    cancel2.getString(tag::OrigClOrdID)};
    REQUIRE(std::find(cancelledIds.begin(), cancelledIds.end(), order1Id) != cancelledIds.end());
    REQUIRE(std::find(cancelledIds.begin(), cancelledIds.end(), order2Id) != cancelledIds.end());

    for (int i = 0; i < 50; ++i) {
        if (runner.snapshot().status == EngineStatus::Idle) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(runner.snapshot().status == EngineStatus::Idle);
    REQUIRE(runner.snapshot().activeOrderCount == 0);

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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
    fill1.set(tag::PosMaintRptID, "P" + std::string(order1ClOrdId));
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

    // The fixed range's Buy Stop (4337) is below the market (4342) here, so
    // it waits for the price; the Sell Stop is what goes out first.
    const FixMessage cycle2Order1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(cycle2Order1.getInt(tag::OrdType) == 3);
    REQUIRE(cycle2Order1.getDouble(tag::StopPx) == Catch::Approx(4336.00));

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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
    const JoinOnExit joinOnExit{runner, runnerThread};  // a failed REQUIRE must not leave the thread running

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
