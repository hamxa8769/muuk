#pragma once

// A minimal in-process FIX TCP server used only by tests, so
// FixSession/LiveRunner behavior can be verified without any real broker
// connection. It is a dumb byte pipe: callers build complete FixMessage
// responses themselves (including header fields) and hand them to
// sendToClient(); the server does no protocol logic of its own.
//
// Accepts multiple connections (LiveRunner always opens exactly two: Trade
// then Quote, in that order — see live_runner.cpp), indexed 0, 1, ... in
// the order they connect.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "muuk/ctrader_fix/fix_message.hpp"

namespace muuk::ctrader_fix::test {

class MockFixServer {
public:
    MockFixServer();
    ~MockFixServer();

    MockFixServer(const MockFixServer&) = delete;
    MockFixServer& operator=(const MockFixServer&) = delete;

    // The ephemeral port this server bound to on 127.0.0.1, as a string —
    // pass this as FixSession's `port` argument.
    [[nodiscard]] std::string port() const { return port_; }

    // Returns the oldest message of the given MsgType on `connectionIndex`
    // that no earlier call has returned, waiting for one to arrive; throws
    // on timeout. Messages of other types or connections are never skipped
    // over, so the order of the test's waits doesn't have to match the
    // exact arrival order across connections.
    [[nodiscard]] FixMessage waitForMessage(const std::string& msgType, std::size_t connectionIndex,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(2));

    // Sends a fully-formed message (encode()'d as-is) to one connected
    // client. Throws if that connection isn't established yet.
    void sendToClient(std::size_t connectionIndex, const FixMessage& msg);

    // Simulates a dropped connection.
    void closeConnection(std::size_t connectionIndex);

private:
    struct Connection {
        asio::ip::tcp::socket socket;
        std::thread reader;
        std::string readBuffer;
        std::mutex writeMutex;

        explicit Connection(asio::io_context& io) : socket(io) {}
    };

    void acceptLoop();
    void readLoop(std::size_t index);

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::string port_;
    std::thread acceptThread_;

    std::mutex connectionsMutex_;
    std::condition_variable connectionsCv_;
    std::vector<std::unique_ptr<Connection>> connections_;

    struct ReceivedMessage {
        std::size_t connectionIndex;
        FixMessage message;
        bool consumed = false;  // already returned by waitForMessage
    };
    std::mutex receivedMutex_;
    std::condition_variable receivedCv_;
    std::vector<ReceivedMessage> received_;

    std::atomic<bool> running_{true};
};

}  // namespace muuk::ctrader_fix::test
