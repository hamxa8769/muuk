#pragma once

// Shared by the LiveRunner test files: a test config pointed at a
// MockFixServer, and the server-side messages every test needs.

#include <cstddef>
#include <string>
#include <thread>

#include "muuk/ctrader_fix/fix_message.hpp"
#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "mock_fix_server.hpp"

namespace muuk::ctrader_fix::test {

using namespace muuk::core;

constexpr std::size_t kTradeConn = 0;  // LiveRunner connects Trade first, then Quote (live_runner.cpp)
constexpr std::size_t kQuoteConn = 1;

inline SymbolSpec xauusdSpec() {
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

inline LiveConfig testConfig(const MockFixServer& server) {
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
    cfg.connection.syncPositionsOnConnect = false;  // the mock broker only answers it in the sync test
    return cfg;
}

inline FixMessage serverHeaderMessage(const std::string& msgType, int seqNum) {
    FixMessage msg(msgType);
    msg.set(tag::SenderCompID, std::string("CSERVER"));
    msg.set(tag::TargetCompID, std::string("demo.testbroker.12345"));
    msg.set(tag::MsgSeqNum, static_cast<int64_t>(seqNum));
    msg.set(tag::SendingTime, nowUtcTimestamp());
    return msg;
}

inline FixMessage buildSecurityListResponse() {
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


// Stops and joins a test's runner thread however the test ends. Without
// it, a failed REQUIRE destroys a still-joinable std::thread, which
// aborts the whole test run instead of reporting the failure.
struct JoinOnExit {
    LiveRunner& runner;
    std::thread& thread;
    JoinOnExit(LiveRunner& r, std::thread& t) : runner(r), thread(t) {}
    ~JoinOnExit() {
        runner.stop();
        if (thread.joinable()) thread.join();
    }
    JoinOnExit(const JoinOnExit&) = delete;
    JoinOnExit& operator=(const JoinOnExit&) = delete;
};

}  // namespace muuk::ctrader_fix::test
