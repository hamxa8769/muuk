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

    // --- A harmless in-range tick ---
    FixMessage snapshot = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, 2);
    snapshot.set(tag::Symbol, std::string("1"));
    snapshot.set(tag::NoMDEntries, int64_t{2});
    snapshot.set(tag::MDEntryType, std::string("0"));
    snapshot.set(tag::MDEntryPx, std::string("4336.50"));
    snapshot.set(tag::MDEntryType, std::string("1"));
    snapshot.set(tag::MDEntryPx, std::string("4336.52"));
    server.sendToClient(kQuoteConn, snapshot);

    // --- Broker fills the first (Buy Stop) order; engine should re-arm it ---
    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, 3);
    fill.set(tag::OrderID, std::string("9001"));
    fill.set(tag::ClOrdID, order1ClOrdId);
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, std::string("4337.00"));
    fill.set(tag::LastQty, std::string("0.01"));
    fill.set(tag::CumQty, std::string("0.01"));
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);

    const FixMessage order3 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(order3.getInt(tag::Side) == 1);
    REQUIRE(order3.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(order3.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(order3.getString(tag::ClOrdID) != order1ClOrdId);

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
