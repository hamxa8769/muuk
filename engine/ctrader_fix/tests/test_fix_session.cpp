#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/fix_session.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;

namespace {

FixCredentials testCredentials() {
    FixCredentials creds;
    creds.senderCompId = "demo.testbroker.12345";
    creds.targetCompId = "CSERVER";
    creds.targetSubId = "TRADE";
    creds.senderSubId = "Trade";
    creds.username = "12345";
    creds.password = "pass";
    creds.heartBtIntSeconds = 1;
    return creds;
}

FixMessage buildServerMessage(const std::string& msgType, int seqNum) {
    FixMessage msg(msgType);
    msg.set(tag::SenderCompID, std::string("CSERVER"));
    msg.set(tag::TargetCompID, std::string("demo.testbroker.12345"));
    msg.set(tag::MsgSeqNum, static_cast<int64_t>(seqNum));
    msg.set(tag::SendingTime, nowUtcTimestamp());
    return msg;
}

// Runs a client-side logon handshake against `server` and returns the
// connected, logged-on session. Fails the test (via REQUIRE, on the main
// thread) if the server never receives a Logon within the timeout or if
// start() throws.
std::unique_ptr<FixSession> logOnAgainstMockServer(MockFixServer& server, FixSession::MessageHandler onAppMsg = {},
                                                     FixSession::DisconnectHandler onDisconnect = {}) {
    auto session =
        std::make_unique<FixSession>("127.0.0.1", server.port(), testCredentials(), std::move(onAppMsg));
    if (onDisconnect) session->setDisconnectHandler(std::move(onDisconnect));

    std::exception_ptr caught;
    std::thread starter([&] {
        try {
            session->start(2s);
        } catch (...) {
            caught = std::current_exception();
        }
    });
    (void)server.waitForMessage(msgtype::Logon, 0);
    server.sendToClient(0, buildServerMessage(msgtype::Logon, 1));
    starter.join();

    if (caught) std::rethrow_exception(caught);
    return session;
}

}  // namespace

TEST_CASE("FixSession logs on successfully against a scripted server") {
    MockFixServer server;
    auto session = logOnAgainstMockServer(server);
    REQUIRE(session->isLoggedOn());
    session->stop();
}

TEST_CASE("FixSession::start() throws when the server rejects Logon with a Logout") {
    MockFixServer server;
    FixSession session("127.0.0.1", server.port(), testCredentials(), {});

    std::exception_ptr caught;
    std::thread starter([&] {
        try {
            session.start(2s);
        } catch (...) {
            caught = std::current_exception();
        }
    });
    (void)server.waitForMessage(msgtype::Logon, 0);
    FixMessage reject = buildServerMessage(msgtype::Logout, 1);
    reject.set(tag::Text, std::string("bad credentials"));
    server.sendToClient(0, reject);
    starter.join();

    REQUIRE(caught != nullptr);
    bool wasFixSessionError = false;
    try {
        std::rethrow_exception(caught);
    } catch (const FixSessionError&) {
        wasFixSessionError = true;
    } catch (...) {
    }
    REQUIRE(wasFixSessionError);
    REQUIRE_FALSE(session.isLoggedOn());
}

TEST_CASE("FixSession invokes the disconnect handler when the server drops the connection") {
    MockFixServer server;
    std::atomic<bool> disconnected{false};
    std::string reasonSeen;

    auto session = logOnAgainstMockServer(server, {}, [&](const std::string& reason) {
        reasonSeen = reason;
        disconnected = true;
    });

    server.closeConnection(0);

    for (int i = 0; i < 50 && !disconnected.load(); ++i) {
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(disconnected.load());
    REQUIRE_FALSE(session->isLoggedOn());
}

TEST_CASE("FixSession answers TestRequest with a Heartbeat carrying the same TestReqID") {
    MockFixServer server;
    auto session = logOnAgainstMockServer(server);

    FixMessage testRequest = buildServerMessage(msgtype::TestRequest, 2);
    testRequest.set(tag::TestReqID, std::string("tr-1"));
    server.sendToClient(0, testRequest);

    const FixMessage heartbeat = server.waitForMessage(msgtype::Heartbeat, 0, 2s);
    REQUIRE(heartbeat.getString(tag::TestReqID) == "tr-1");

    session->stop();
}

TEST_CASE("FixSession sends periodic heartbeats on its own timer") {
    MockFixServer server;
    auto session = logOnAgainstMockServer(server);  // heartBtIntSeconds = 1

    // The timer-driven heartbeat (not a TestRequest reply) should arrive
    // within roughly one interval.
    REQUIRE_NOTHROW(server.waitForMessage(msgtype::Heartbeat, 0, 2s));

    session->stop();
}

TEST_CASE("FixSession forwards non-session application messages to the caller's handler") {
    MockFixServer server;
    std::atomic<int> appMessagesSeen{0};
    std::string lastMsgType;

    auto session = logOnAgainstMockServer(server, [&](const FixMessage& msg) {
        lastMsgType = msg.msgType();
        ++appMessagesSeen;
    });

    FixMessage exec = buildServerMessage(msgtype::ExecutionReport, 2);
    exec.set(tag::OrderID, std::string("101"));
    exec.set(tag::ExecType, std::string("0"));
    exec.set(tag::OrdStatus, std::string("0"));
    server.sendToClient(0, exec);

    for (int i = 0; i < 50 && appMessagesSeen.load() == 0; ++i) {
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(appMessagesSeen.load() == 1);
    REQUIRE(lastMsgType == msgtype::ExecutionReport);

    session->stop();
}
