#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <asio.hpp>

#include "muuk/ctrader_fix/fix_message.hpp"

namespace muuk::ctrader_fix {

class FixSessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One cTrader FIX session's credentials. A live connection needs two
// FixSession instances (Quote, Trade) sharing the same account identity
// but differing in targetSubId/senderSubId — see docs/ctrader-fix.md.
struct FixCredentials {
    std::string senderCompId;         // "<environment>.<BrokerUID>.<TraderLogin>"
    std::string targetCompId = "CSERVER";
    std::string targetSubId;          // "QUOTE" or "TRADE"
    std::string senderSubId;          // "Quote" for the QUOTE session; any string for TRADE
    std::string username;             // numeric trader login
    std::string password;
    int heartBtIntSeconds = 30;
};

// A single FIX 4.4 session over one TCP connection: handles the session-
// level protocol (Logon/Heartbeat/TestRequest/Logout, per spec §5.3) and
// hands every other (application) message to the caller's handler.
//
// Deliberately synchronous/thread-based rather than a full async state
// machine: a dedicated reader thread blocks on the socket and dispatches,
// writes are synchronous under a mutex, and a heartbeat thread sends on a
// timer. This is simple to reason about and entirely adequate for this
// client's message rate (heartbeats every ~30s, order/quote traffic far
// below anything that needs an event-loop).
//
// Known scope limits (see docs/ctrader-fix.md): no FIX-level message-store
// replay on ResendRequest, and incoming sequence-number gaps are not
// detected — every session logs on with ResetSeqNumFlag=Y, so this only
// matters within a single connection's lifetime, and a dropped connection
// is surfaced via the disconnect handler for the caller (LiveRunner) to
// react to rather than silently recovered here.
class FixSession {
public:
    using MessageHandler = std::function<void(const FixMessage&)>;
    using DisconnectHandler = std::function<void(const std::string& reason)>;

    FixSession(std::string host, std::string port, FixCredentials credentials,
               MessageHandler onApplicationMessage);
    ~FixSession();

    FixSession(const FixSession&) = delete;
    FixSession& operator=(const FixSession&) = delete;

    // Connects and blocks until the server acknowledges Logon (or rejects
    // it, or `timeout` elapses) — throws FixSessionError on failure. Starts
    // the background reader/heartbeat threads on success.
    void start(std::chrono::milliseconds timeout = std::chrono::seconds(10));

    // Sends Logout (if currently logged on) and stops the background
    // threads. Safe to call multiple times / without a prior start().
    void stop();

    // Called at most once, from the reader thread, if the session ends
    // unexpectedly (socket error, malformed message, or a Logout the
    // caller didn't initiate) after having been logged on. Not called for
    // an orderly stop(). Must be called before start() — it is read
    // without synchronization from the reader thread once running.
    void setDisconnectHandler(DisconnectHandler handler) { onDisconnect_ = std::move(handler); }

    // Fills in the standard header (SenderCompID/TargetCompID/
    // TargetSubID/SenderSubID/MsgSeqNum/SendingTime) and writes the
    // message to the wire. Thread-safe.
    void send(const FixMessage& appMessage);

    [[nodiscard]] bool isLoggedOn() const { return loggedOn_.load(); }

private:
    void readLoop();
    void heartbeatLoop();
    void handleIncoming(const FixMessage& msg);
    void onSessionEnded(const std::string& reason);

    std::string host_;
    std::string port_;
    FixCredentials credentials_;
    MessageHandler onApplicationMessage_;
    DisconnectHandler onDisconnect_;

    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    std::string readBuffer_;

    std::atomic<int> outgoingSeqNum_{1};
    std::atomic<bool> loggedOn_{false};
    std::atomic<bool> running_{false};

    std::mutex writeMutex_;

    std::mutex logonMutex_;
    std::condition_variable logonCv_;
    bool logonSettled_ = false;
    std::optional<std::string> logonFailureReason_;

    std::mutex shutdownMutex_;
    std::condition_variable shutdownCv_;

    std::thread readThread_;
    std::thread heartbeatThread_;
};

}  // namespace muuk::ctrader_fix
