#include "muuk/ctrader_fix/fix_session.hpp"

#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"

namespace muuk::ctrader_fix {

FixSession::FixSession(std::string host, std::string port, FixCredentials credentials,
                        MessageHandler onApplicationMessage)
    : host_(std::move(host)),
      port_(std::move(port)),
      credentials_(std::move(credentials)),
      onApplicationMessage_(std::move(onApplicationMessage)),
      socket_(io_) {}

FixSession::~FixSession() { stop(); }

void FixSession::start(std::chrono::milliseconds timeout) {
    if (running_.load()) {
        throw FixSessionError("FixSession::start() called while already running");
    }

    asio::ip::tcp::resolver resolver(io_);
    asio::error_code ec;
    const auto endpoints = resolver.resolve(host_, port_, ec);
    if (ec) {
        throw FixSessionError("failed to resolve " + host_ + ":" + port_ + ": " + ec.message());
    }
    asio::connect(socket_, endpoints, ec);
    if (ec) {
        throw FixSessionError("failed to connect to " + host_ + ":" + port_ + ": " + ec.message());
    }

    outgoingSeqNum_ = 1;
    loggedOn_ = false;
    running_ = true;
    logonSettled_ = false;
    logonFailureReason_.reset();

    readThread_ = std::thread([this] { readLoop(); });

    send(buildLogon(credentials_.username, credentials_.password, credentials_.heartBtIntSeconds));

    {
        std::unique_lock lock(logonMutex_);
        const bool settled = logonCv_.wait_for(lock, timeout, [this] { return logonSettled_; });
        if (!settled) {
            running_ = false;
            socket_.close();
            if (readThread_.joinable()) readThread_.join();
            throw FixSessionError("timed out waiting for Logon acknowledgement from " + host_);
        }
        if (logonFailureReason_) {
            running_ = false;
            socket_.close();
            if (readThread_.joinable()) readThread_.join();
            throw FixSessionError("Logon rejected: " + *logonFailureReason_);
        }
    }

    heartbeatThread_ = std::thread([this] { heartbeatLoop(); });
}

void FixSession::stop() {
    // Note: `wasRunning` only gates the Logout send below — the thread
    // joins and socket teardown must always run unconditionally, even when
    // the session already ended on its own (e.g. onSessionEnded() from a
    // dropped connection already set running_ = false on the reader
    // thread). Skipping them in that case used to leave readThread_/
    // heartbeatThread_ still joinable when this FixSession was destroyed,
    // and a joinable std::thread's destructor calls std::terminate().
    const bool wasRunning = running_.exchange(false);

    if (wasRunning && loggedOn_.exchange(false)) {
        try {
            send(buildLogout());
        } catch (const std::exception&) {
            // best-effort: the socket may already be broken.
        }
    }

    shutdownCv_.notify_all();

    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    if (heartbeatThread_.joinable()) heartbeatThread_.join();
    if (readThread_.joinable()) readThread_.join();
}

void FixSession::send(const FixMessage& appMessage) {
    std::scoped_lock lock(writeMutex_);

    FixMessage full(appMessage.msgType());
    full.set(tag::SenderCompID, credentials_.senderCompId);
    full.set(tag::TargetCompID, credentials_.targetCompId);
    full.set(tag::MsgSeqNum, static_cast<int64_t>(outgoingSeqNum_++));
    full.set(tag::SendingTime, nowUtcTimestamp());
    if (!credentials_.targetSubId.empty()) full.set(tag::TargetSubID, credentials_.targetSubId);
    if (!credentials_.senderSubId.empty()) full.set(tag::SenderSubID, credentials_.senderSubId);
    for (const auto& [t, v] : appMessage.fields()) full.set(t, v);

    const std::string encoded = full.encode();
    asio::error_code ec;
    asio::write(socket_, asio::buffer(encoded), ec);
    if (ec) {
        throw FixSessionError("FIX write failed: " + ec.message());
    }
}

void FixSession::readLoop() {
    char chunk[4096];
    while (running_.load()) {
        asio::error_code ec;
        const std::size_t n = socket_.read_some(asio::buffer(chunk), ec);
        if (ec) {
            if (running_.load()) onSessionEnded("socket read error: " + ec.message());
            return;
        }
        readBuffer_.append(chunk, n);

        while (true) {
            FixMessage msg("");
            std::size_t consumed = 0;
            bool ok = false;
            try {
                ok = FixMessage::tryDecode(readBuffer_, msg, consumed);
            } catch (const FixMessageError& e) {
                onSessionEnded(std::string("FIX decode error: ") + e.what());
                return;
            }
            if (!ok) break;
            readBuffer_.erase(0, consumed);
            handleIncoming(msg);
        }
    }
}

void FixSession::onSessionEnded(const std::string& reason) {
    const bool wasLoggedOn = loggedOn_.exchange(false);
    running_ = false;

    {
        std::scoped_lock lock(logonMutex_);
        if (!logonSettled_) {
            logonFailureReason_ = reason;
            logonSettled_ = true;
        }
    }
    logonCv_.notify_all();
    shutdownCv_.notify_all();

    if (wasLoggedOn && onDisconnect_) onDisconnect_(reason);
}

void FixSession::heartbeatLoop() {
    while (running_.load()) {
        std::unique_lock lock(shutdownMutex_);
        const bool stopped = shutdownCv_.wait_for(lock, std::chrono::seconds(credentials_.heartBtIntSeconds),
                                                    [this] { return !running_.load(); });
        if (stopped || !running_.load()) return;
        lock.unlock();
        try {
            send(buildHeartbeat());
        } catch (const std::exception&) {
            return;  // readLoop will observe the same broken socket and report it
        }
    }
}

void FixSession::handleIncoming(const FixMessage& msg) {
    const std::string& type = msg.msgType();

    if (type == msgtype::Logon) {
        std::scoped_lock lock(logonMutex_);
        loggedOn_ = true;
        logonSettled_ = true;
        logonCv_.notify_all();
        return;
    }

    if (type == msgtype::Logout) {
        const std::string text = msg.getOptionalString(tag::Text).value_or("session closed by server");
        onSessionEnded(text);
        return;
    }

    if (type == msgtype::TestRequest) {
        const auto testReqId = msg.getOptionalString(tag::TestReqID);
        try {
            send(buildHeartbeat(testReqId));
        } catch (const std::exception&) {
        }
        return;
    }

    if (type == msgtype::Heartbeat || type == msgtype::SequenceReset || type == msgtype::ResendRequest) {
        // Liveness/session-sequencing messages: no application action
        // needed for this client's scope (see class comment).
        return;
    }

    if (onApplicationMessage_) onApplicationMessage_(msg);
}

}  // namespace muuk::ctrader_fix
