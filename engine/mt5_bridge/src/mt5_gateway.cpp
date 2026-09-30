#include "muuk/mt5_bridge/mt5_gateway.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "muuk/ctrader_fix/messages.hpp"
#include "muuk/ctrader_fix/tags.hpp"

namespace muuk::mt5_bridge {

using muuk::ctrader_fix::FixMessage;
namespace tag = muuk::ctrader_fix::tag;
namespace msgtype = muuk::ctrader_fix::msgtype;

namespace {
constexpr double kWireQtyPerLot = 100.0;  // same convention as cTrader FIX (see live_runner.cpp)
constexpr const char* kSymbolId = "1";

std::string jsonString(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null()) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : v.dump();
}

double jsonNumber(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key) || !j.at(key).is_number()) return fallback;
    return j.at(key).get<double>();
}
}  // namespace

Mt5Gateway::Mt5Gateway(GatewayConfig config, LogHandler log) : config_(std::move(config)), log_(std::move(log)) {}

Mt5Gateway::~Mt5Gateway() { stop(); }

void Mt5Gateway::log(const std::string& line) const {
    if (log_) {
        log_("[mt5] " + line);
    } else {
        std::cerr << "[mt5] " << line << "\n";
    }
}

void Mt5Gateway::start() {
    if (running_.exchange(true)) return;
#ifdef _WIN32
    // The pipe first: FILE_FLAG_FIRST_PIPE_INSTANCE makes a second copy of
    // the bridge fail here instead of sharing the name.
    firstPipe_ = createPipe();
    if (firstPipe_ == nullptr) {
        running_ = false;
        throw std::runtime_error("MT5 bridge: can't open the pipe \\\\.\\pipe\\" + config_.pipeName +
                                 " -- is another copy of Muuk already running the bridge?");
    }
#endif
    try {
        const auto address = asio::ip::make_address(config_.bindAddress);
        fixAcceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_);
        const asio::ip::tcp::endpoint ep(address, config_.fixPort);
        fixAcceptor_->open(ep.protocol());
        // No SO_REUSEADDR: a second copy of the gateway must fail here, not share the port.
        fixAcceptor_->bind(ep);
        fixAcceptor_->listen();
        fixPort_ = fixAcceptor_->local_endpoint().port();
    } catch (const std::exception& ex) {
        running_ = false;
        fixAcceptor_.reset();
#ifdef _WIN32
        CloseHandle(static_cast<HANDLE>(firstPipe_));
        firstPipe_ = nullptr;
#endif
        throw std::runtime_error("MT5 bridge: can't listen on " + config_.bindAddress + " port " +
                                 std::to_string(config_.fixPort) + " (" + ex.what() +
                                 ") -- is another copy of Muuk already running the bridge?");
    }
    log("bridge ready: MetaTrader 5 on pipe " + config_.pipeName + ", Muuk on port " + std::to_string(fixPort_));
    eaThread_ = std::thread([this] { eaPipeLoop(); });
    fixAcceptThread_ = std::thread([this] { fixAcceptLoop(); });
}

void Mt5Gateway::stop() {
    if (!running_.exchange(false)) return;
    asio::error_code ec;
    if (fixAcceptor_) fixAcceptor_->close(ec);
    std::vector<FixConnPtr> conns;
    {
        std::scoped_lock lock(mutex_);
        conns = fixConns_;
    }
    for (const auto& c : conns) {
        c->open = false;
        c->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        c->socket.close(ec);
    }
#ifdef _WIN32
    if (eaThread_.joinable()) {
        // Wake the pipe thread: a blocked read is cancelled; a wait for a
        // client ends when we connect to our own pipe for a moment.
        const std::wstring name = pipePath();
        while (!eaThreadDone_.load()) {
            CancelSynchronousIo(static_cast<HANDLE>(eaThread_.native_handle()));
            HANDLE self = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (self != INVALID_HANDLE_VALUE) CloseHandle(self);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        eaThread_.join();
    }
#endif
    if (fixAcceptThread_.joinable()) fixAcceptThread_.join();
    {
        std::scoped_lock lock(mutex_);
        conns = fixConns_;
    }
    for (const auto& c : conns) {
        if (c->reader.joinable()) c->reader.join();
    }
    std::scoped_lock lock(mutex_);
    fixConns_.clear();
    tradeConn_.reset();
    quoteConn_.reset();
    outbox_.clear();
    status_.connected = false;
    status_.ready = false;
}

EaStatus Mt5Gateway::status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

// ---------------------------------------------------------------------------
// EA side: a named pipe. The EA polls: every ~25 ms it sends one frame with
// what happened (hello, prices, account, fills...) and gets one frame back
// with the commands waiting for it. A frame = 4-byte little-endian length +
// that many bytes of JSON lines. No MT5 setting is needed for a pipe (a TCP
// socket needs 127.0.0.1 in MT5's WebRequest list).

#ifdef _WIN32
std::wstring Mt5Gateway::pipePath() const {
    const std::string full = "\\\\.\\pipe\\" + config_.pipeName;
    return std::wstring(full.begin(), full.end());
}

void* Mt5Gateway::createPipe() const {
    const std::wstring name = pipePath();
    HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                1 << 16, 1 << 16, 0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

namespace {
bool readExact(HANDLE h, void* buf, DWORD size) {
    auto* p = static_cast<char*>(buf);
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, size, &got, nullptr) || got == 0) return false;
        p += got;
        size -= got;
    }
    return true;
}

bool writeExact(HANDLE h, const void* buf, DWORD size) {
    const auto* p = static_cast<const char*>(buf);
    while (size > 0) {
        DWORD put = 0;
        if (!WriteFile(h, p, size, &put, nullptr) || put == 0) return false;
        p += put;
        size -= put;
    }
    return true;
}
}  // namespace

void Mt5Gateway::eaPipeLoop() {
    HANDLE pipe = static_cast<HANDLE>(std::exchange(firstPipe_, nullptr));
    while (running_.load()) {
        if (pipe == nullptr) {
            pipe = static_cast<HANDLE>(createPipe());
            if (pipe == nullptr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!running_.load()) break;
        if (!connected) {
            CloseHandle(pipe);
            pipe = nullptr;
            continue;
        }
        {
            std::scoped_lock lock(mutex_);
            status_ = EaStatus{};
            status_.connected = true;
            outbox_.clear();
        }
        log("MetaTrader 5 connected -- waiting for its hello");
        std::string frame;
        while (running_.load()) {
            uint32_t len = 0;
            if (!readExact(pipe, &len, sizeof(len)) || len > (16u << 20)) break;
            frame.assign(len, '\0');
            if (len > 0 && !readExact(pipe, frame.data(), len)) break;
            std::size_t start = 0;
            while (start < frame.size()) {
                std::size_t end = frame.find('\n', start);
                if (end == std::string::npos) end = frame.size();
                std::string line = frame.substr(start, end - start);
                start = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                try {
                    onEaLine(line);
                } catch (const std::exception& ex) {
                    log(std::string("couldn't handle a line from MT5 (") + ex.what() + "): " + line.substr(0, 200));
                }
            }
            std::string reply;
            {
                std::scoped_lock lock(mutex_);
                for (const auto& c : outbox_) reply += c + "\n";
                outbox_.clear();
            }
            const auto replyLen = static_cast<uint32_t>(reply.size());
            if (!writeExact(pipe, &replyLen, sizeof(replyLen)) ||
                (replyLen > 0 && !writeExact(pipe, reply.data(), replyLen))) {
                break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = nullptr;
        {
            std::scoped_lock lock(mutex_);
            status_.connected = false;
            status_.ready = false;
            outbox_.clear();
        }
        if (running_.load()) {
            log("MetaTrader 5 disconnected -- Muuk's session ends until it is back");
            closeAllFix("MT5 bridge: MetaTrader 5 disconnected (closed, or MuukBridge removed from the chart)");
        }
    }
    if (pipe != nullptr) CloseHandle(pipe);
    eaThreadDone_ = true;
}
#else
void Mt5Gateway::eaPipeLoop() { eaThreadDone_ = true; }
#endif

bool Mt5Gateway::sendEa(const nlohmann::json& cmd) {
    std::scoped_lock lock(mutex_);
    if (!status_.connected) return false;
    outbox_.push_back(cmd.dump());
    return true;
}

void Mt5Gateway::onEaLine(const std::string& line) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const std::exception& ex) {
        log(std::string("bad line from the EA (") + ex.what() + "): " + line.substr(0, 200));
        return;
    }
    const std::string t = jsonString(j, "t");

    if (t == "hello") {
        bool refuse = false;
        {
            std::scoped_lock lock(mutex_);
            status_.ready = true;
            status_.login = jsonString(j, "login");
            status_.server = jsonString(j, "server");
            status_.company = jsonString(j, "company");
            status_.currency = jsonString(j, "currency");
            status_.symbol = jsonString(j, "symbol");
            status_.hedging = j.value("hedging", true);
            status_.digits = static_cast<int>(jsonNumber(j, "digits", 2));
            status_.contractSize = jsonNumber(j, "contract", 100.0);
            status_.volumeMin = jsonNumber(j, "volMin", 0.01);
            status_.volumeStep = jsonNumber(j, "volStep", 0.01);
            status_.volumeMax = jsonNumber(j, "volMax", 100.0);
            status_.leverage = jsonNumber(j, "leverage", 0.0);
            status_.hedgedMarginPercent = jsonNumber(j, "hedgedMargin", -1.0);
            status_.tickSize = jsonNumber(j, "tickSize");
            status_.tickValue = jsonNumber(j, "tickValue");
            status_.marginPerLot = jsonNumber(j, "marginPerLot");
            status_.magic = static_cast<int>(jsonNumber(j, "magic", 0));
            status_.limitOrders = static_cast<int>(jsonNumber(j, "limitOrders", 0));
            status_.tradeMode = jsonString(j, "tradeMode");
            refuse = !status_.hedging;
        }
        const EaStatus s = status();
        log("MT5 account " + s.login + " on " + s.server + " (" + s.company + "), " + s.currency + ", 1:" +
            std::to_string(static_cast<int>(s.leverage)) + ", symbol " + s.symbol + ", min lot " +
            std::to_string(s.volumeMin).substr(0, 4) + (s.hedging ? ", hedging" : ", NETTING"));
        if (refuse) {
            log("this MT5 account is a netting account: an opposite order would close the position instead of "
                "locking it -- the bridge refuses it. Open a hedging account.");
            closeAllFix("MT5 bridge: the MT5 account is a netting account -- Muuk needs a hedging account");
        }
        return;
    }
    if (t == "log") {
        log("EA: " + jsonString(j, "text"));
        return;
    }
    if (t == "tick") {
        const double bid = jsonNumber(j, "bid");
        const double ask = jsonNumber(j, "ask");
        {
            std::scoped_lock lock(mutex_);
            status_.bid = bid;
            status_.ask = ask;
        }
        if (bid > 0.0 && ask > 0.0) {
            if (const auto q = quoteConn()) sendQuote(q, bid, ask);
        }
        return;
    }
    if (t == "account") {
        {
            std::scoped_lock lock(mutex_);
            status_.accountKnown = true;
            status_.balance = jsonNumber(j, "balance");
            status_.equity = jsonNumber(j, "equity");
            status_.margin = jsonNumber(j, "margin");
            status_.freeMargin = jsonNumber(j, "free");
            if (jsonNumber(j, "marginPerLot") > 0.0) status_.marginPerLot = jsonNumber(j, "marginPerLot");
            if (jsonNumber(j, "tickValue") > 0.0) status_.tickValue = jsonNumber(j, "tickValue");
            lastCosts_ = j.contains("costs") && j.at("costs").is_array() ? j.at("costs") : nlohmann::json::array();
        }
        if (const auto tc = tradeConn()) sendAccountInfo(tc, j.value("costs", nlohmann::json::array()));
        return;
    }

    const auto tc = tradeConn();
    if (t == "ack") {
        if (!tc) return;
        sendFix(tc, executionReport(jsonString(j, "id"), "0", "0", jsonString(j, "ticket")));
        return;
    }
    if (t == "reject") {
        if (tc) rejectOrder(tc, jsonString(j, "id"), jsonString(j, "text"));
        return;
    }
    if (t == "fill") {
        if (!tc) return;
        const std::string id = jsonString(j, "id");
        const double lots = jsonNumber(j, "volume");
        FixMessage er = executionReport(id, "F", "2");
        er.set(tag::AvgPx, jsonNumber(j, "price"), 5);
        er.set(tag::LastQty, lots * kWireQtyPerLot, 2);
        er.set(tag::CumQty, lots * kWireQtyPerLot, 2);
        er.set(tag::LeavesQty, std::string("0"));
        const std::string pos = jsonString(j, "pos");
        if (!pos.empty()) er.set(tag::PosMaintRptID, pos);
        sendFix(tc, er);
        std::scoped_lock lock(mutex_);
        orders_.erase(id);
        return;
    }
    if (t == "cancelled") {
        if (!tc) return;
        const std::string id = jsonString(j, "id");
        sendFix(tc, executionReport(id, "4", "4"));
        std::scoped_lock lock(mutex_);
        orders_.erase(id);
        return;
    }
    if (t == "cancel_reject") {
        if (!tc) return;
        FixMessage r(msgtype::OrderCancelReject);
        r.set(tag::OrderID, std::string("NONE"));
        r.set(tag::ClOrdID, jsonString(j, "id"));
        r.set(tag::OrigClOrdID, jsonString(j, "orig"));
        r.set(tag::OrdStatus, std::string("8"));
        r.set(tag::CxlRejResponseTo, std::string("1"));
        r.set(tag::Text, jsonString(j, "text"));
        sendFix(tc, r);
        return;
    }
    if (t == "positions") {
        const std::string req = jsonString(j, "req");
        FixConnPtr to;
        {
            std::scoped_lock lock(mutex_);
            if (const auto it = positionReqs_.find(req); it != positionReqs_.end()) {
                to = it->second;
                positionReqs_.erase(it);
            }
        }
        if (!to) to = tc;
        if (!to) return;
        const auto list = j.value("list", nlohmann::json::array());
        if (list.empty()) {
            FixMessage none(msgtype::PositionReport);
            none.set(tag::PosReqID, req);
            none.set(tag::PosReqResult, std::string("2"));
            sendFix(to, none);
            return;
        }
        for (const auto& p : list) {
            const bool buy = p.value("buy", true);
            const double qty = jsonNumber(p, "volume") * kWireQtyPerLot;
            FixMessage r(msgtype::PositionReport);
            r.set(tag::PosReqID, req);
            r.set(tag::PosMaintRptID, jsonString(p, "pos"));
            r.set(tag::TotalNumPosReports, static_cast<int64_t>(list.size()));
            r.set(tag::PosReqResult, std::string("0"));
            r.set(tag::Symbol, std::string(kSymbolId));
            r.set(tag::NoPositions, int64_t{1});
            r.set(tag::LongQty, buy ? qty : 0.0, 2);
            r.set(tag::ShortQty, buy ? 0.0 : qty, 2);
            r.set(tag::SettlPrice, jsonNumber(p, "price"), 5);
            sendFix(to, r);
        }
        return;
    }
    if (t == "orders") {
        const std::string req = jsonString(j, "req");
        FixConnPtr to;
        {
            std::scoped_lock lock(mutex_);
            if (const auto it = orderReqs_.find(req); it != orderReqs_.end()) {
                to = it->second;
                orderReqs_.erase(it);
            }
        }
        if (!to) to = tc;
        if (!to) return;
        const auto list = j.value("list", nlohmann::json::array());
        if (list.empty()) {
            FixMessage none(msgtype::BusinessMessageReject);
            none.set(tag::RefMsgType, std::string(msgtype::OrderMassStatusRequest));
            none.set(tag::BusinessRejectRefID, req);
            none.set(tag::BusinessRejectReason, int64_t{0});
            none.set(tag::Text, std::string("ORDER_NOT_FOUND:no orders found"));
            sendFix(to, none);
            return;
        }
        for (const auto& o : list) {
            FixMessage r(msgtype::ExecutionReport);
            r.set(tag::OrderID, jsonString(o, "ticket"));
            r.set(tag::ClOrdID, jsonString(o, "id"));
            r.set(tag::MassStatusReqID, req);
            r.set(tag::TotNumReports, static_cast<int64_t>(list.size()));
            r.set(tag::ExecType, std::string("I"));
            r.set(tag::OrdStatus, std::string("0"));
            r.set(tag::Symbol, std::string(kSymbolId));
            r.set(tag::Side, static_cast<int64_t>(o.value("buy", true) ? 1 : 2));
            r.set(tag::OrderQty, jsonNumber(o, "volume") * kWireQtyPerLot, 2);
            r.set(tag::OrdType, int64_t{3});
            r.set(tag::StopPx, jsonNumber(o, "price"), 5);
            sendFix(to, r);
        }
        return;
    }
    log("unknown line from the EA: " + line.substr(0, 200));
}

// ---------------------------------------------------------------------------
// FIX side

void Mt5Gateway::fixAcceptLoop() {
    while (running_.load()) {
        auto conn = std::make_shared<FixConn>(io_);
        asio::error_code ec;
        fixAcceptor_->accept(conn->socket, ec);
        if (ec) return;
        conn->socket.set_option(asio::ip::tcp::no_delay(true), ec);
        std::vector<FixConnPtr> ended;
        {
            std::scoped_lock lock(mutex_);
            // Forget connections that already ended (joined below, outside the lock).
            std::vector<FixConnPtr> keep;
            for (auto& c : fixConns_) (c->open.load() ? keep : ended).push_back(c);
            fixConns_ = std::move(keep);
            fixConns_.push_back(conn);
            conn->reader = std::thread([this, conn] { fixReadLoop(conn); });
        }
        for (auto& c : ended) {
            if (c->reader.joinable()) c->reader.join();
        }
    }
}

void Mt5Gateway::fixReadLoop(FixConnPtr conn) {
    std::string buffer;
    char chunk[8192];
    while (running_.load() && conn->open.load()) {
        asio::error_code ec;
        const std::size_t n = conn->socket.read_some(asio::buffer(chunk), ec);
        if (ec) break;
        buffer.append(chunk, n);
        try {
            while (true) {
                FixMessage msg("");
                std::size_t consumed = 0;
                if (!FixMessage::tryDecode(buffer, msg, consumed)) break;
                buffer.erase(0, consumed);
                onFixMessage(conn, msg);
            }
        } catch (const std::exception& ex) {
            log(std::string("bad FIX message from Muuk: ") + ex.what());
            break;
        }
    }
    conn->open = false;
    std::scoped_lock lock(mutex_);
    if (tradeConn_ == conn) tradeConn_.reset();
    if (quoteConn_ == conn) quoteConn_.reset();
}

Mt5Gateway::FixConnPtr Mt5Gateway::tradeConn() const {
    std::scoped_lock lock(mutex_);
    return tradeConn_ && tradeConn_->open.load() ? tradeConn_ : nullptr;
}

Mt5Gateway::FixConnPtr Mt5Gateway::quoteConn() const {
    std::scoped_lock lock(mutex_);
    return quoteConn_ && quoteConn_->open.load() ? quoteConn_ : nullptr;
}

void Mt5Gateway::sendFix(const FixConnPtr& conn, const FixMessage& body) {
    if (!conn || !conn->open.load()) return;
    std::scoped_lock writeLock(conn->writeMutex);
    FixMessage m(body.msgType());
    m.set(tag::SenderCompID, conn->ourCompId);
    m.set(tag::TargetCompID, conn->theirCompId);
    if (!conn->theirSubId.empty()) m.set(tag::TargetSubID, conn->theirSubId);
    if (!conn->ourSubId.empty()) m.set(tag::SenderSubID, conn->ourSubId);
    m.set(tag::MsgSeqNum, conn->outSeq++);
    m.set(tag::SendingTime, muuk::ctrader_fix::nowUtcTimestamp());
    for (const auto& [t, v] : body.fields()) m.set(t, v);
    const std::string wire = m.encode();
    asio::error_code ec;
    asio::write(conn->socket, asio::buffer(wire), ec);
    if (ec) conn->open = false;
}

void Mt5Gateway::closeFix(const FixConnPtr& conn, const std::string& logoutText) {
    if (!conn) return;
    FixMessage logout(msgtype::Logout);
    logout.set(tag::Text, logoutText);
    sendFix(conn, logout);
    conn->open = false;
    asio::error_code ec;
    conn->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    conn->socket.close(ec);
}

void Mt5Gateway::closeAllFix(const std::string& logoutText) {
    std::vector<FixConnPtr> conns;
    {
        std::scoped_lock lock(mutex_);
        conns = fixConns_;
    }
    for (const auto& c : conns) {
        if (c->open.load()) closeFix(c, logoutText);
    }
}

FixMessage Mt5Gateway::executionReport(const std::string& clOrdId, const char* execType, const char* ordStatus,
                                       const std::string& orderId) const {
    FixMessage er(msgtype::ExecutionReport);
    er.set(tag::OrderID, orderId.empty() ? "M" + clOrdId : orderId);
    er.set(tag::ClOrdID, clOrdId);
    er.set(tag::ExecType, std::string(execType));
    er.set(tag::OrdStatus, std::string(ordStatus));
    er.set(tag::Symbol, std::string(kSymbolId));
    std::scoped_lock lock(mutex_);
    if (const auto it = orders_.find(clOrdId); it != orders_.end()) {
        er.set(tag::Side, static_cast<int64_t>(it->second.side));
        er.set(tag::OrderQty, it->second.qty, 2);
        er.set(tag::OrdType, static_cast<int64_t>(it->second.ordType));
        if (it->second.ordType == 3) er.set(tag::StopPx, it->second.stopPx, 5);
    }
    return er;
}

void Mt5Gateway::rejectOrder(const FixConnPtr& conn, const std::string& clOrdId, const std::string& text) {
    sendFix(conn, executionReport(clOrdId, "8", "8").set(tag::Text, text));
    std::scoped_lock lock(mutex_);
    orders_.erase(clOrdId);
}

void Mt5Gateway::sendQuote(const FixConnPtr& conn, double bid, double ask) {
    FixMessage w(msgtype::MarketDataSnapshotFullRefresh);
    w.set(tag::Symbol, std::string(kSymbolId));
    w.set(tag::NoMDEntries, int64_t{2});
    w.set(tag::MDEntryType, std::string("0"));
    w.set(tag::MDEntryPx, bid, 5);
    w.set(tag::MDEntryType, std::string("1"));
    w.set(tag::MDEntryPx, ask, 5);
    sendFix(conn, w);
}

void Mt5Gateway::sendAccountInfo(const FixConnPtr& conn, const nlohmann::json& costs) {
    const EaStatus s = status();
    if (!s.ready || !s.accountKnown) return;  // never pass on zeros as if they were the account
    FixMessage m(msgtype::MuukAccountInfo);
    m.set(tag::MuukBalance, s.balance, 2);
    m.set(tag::MuukEquity, s.equity, 2);
    m.set(tag::MuukMargin, s.margin, 2);
    m.set(tag::MuukFreeMargin, s.freeMargin, 2);
    m.set(tag::MuukCurrency, s.currency);
    m.set(tag::MuukLeverage, s.leverage, 0);
    m.set(tag::MuukHedgedMarginPercent, s.hedgedMarginPercent, 2);
    m.set(tag::MuukContractSize, s.contractSize, 4);
    m.set(tag::MuukVolumeMin, s.volumeMin, 4);
    m.set(tag::MuukVolumeStep, s.volumeStep, 4);
    m.set(tag::MuukLogin, s.login);
    m.set(tag::MuukServer, s.server);
    m.set(tag::MuukCompany, s.company);
    m.set(tag::MuukHedging, std::string(s.hedging ? "Y" : "N"));
    m.set(tag::MuukSymbol, s.symbol);
    m.set(tag::MuukTickSize, s.tickSize, 8);
    m.set(tag::MuukTickValue, s.tickValue, 8);
    m.set(tag::MuukMarginPerLot, s.marginPerLot, 4);
    m.set(tag::MuukDigits, static_cast<int64_t>(s.digits));
    m.set(tag::MuukVolumeMax, s.volumeMax, 4);
    m.set(tag::MuukLimitOrders, static_cast<int64_t>(s.limitOrders));
    m.set(tag::MuukTradeMode, s.tradeMode);
    m.set(tag::MuukNoPositionCosts, static_cast<int64_t>(costs.size()));
    for (const auto& c : costs) {
        m.set(tag::MuukCostPositionId, jsonString(c, "pos"));
        m.set(tag::MuukCostSwap, jsonNumber(c, "swap"), 2);
        m.set(tag::MuukCostCommission, jsonNumber(c, "comm"), 2);
    }
    sendFix(conn, m);
}

void Mt5Gateway::onFixMessage(const FixConnPtr& conn, const FixMessage& msg) {
    const std::string& type = msg.msgType();

    if (type == msgtype::Logon) {
        conn->theirCompId = msg.getOptionalString(tag::SenderCompID).value_or("MUUK");
        conn->ourCompId = msg.getOptionalString(tag::TargetCompID).value_or("MT5");
        conn->theirSubId = msg.getOptionalString(tag::SenderSubID).value_or("");
        conn->ourSubId = msg.getOptionalString(tag::TargetSubID).value_or("");
        const EaStatus s = status();
        if (!s.connected || !s.ready) {
            closeFix(conn, "MT5 bridge: MetaTrader 5 isn't connected -- open MT5, attach MuukBridge to the " +
                               std::string("gold chart and allow algo trading"));
            return;
        }
        if (!s.hedging) {
            closeFix(conn, "MT5 bridge: the MT5 account is a netting account -- Muuk needs a hedging account");
            return;
        }
        {
            std::scoped_lock lock(mutex_);
            if (conn->ourSubId == "QUOTE") {
                quoteConn_ = conn;
            } else {
                tradeConn_ = conn;
            }
        }
        FixMessage ack(msgtype::Logon);
        ack.set(tag::EncryptMethod, int64_t{0});
        ack.set(tag::HeartBtInt, msg.getOptionalString(tag::HeartBtInt).value_or("30"));
        sendFix(conn, ack);
        if (conn->ourSubId != "QUOTE") {
            nlohmann::json costs;
            {
                std::scoped_lock lock(mutex_);
                costs = lastCosts_;
            }
            sendAccountInfo(conn, costs);
        }
        return;
    }
    if (type == msgtype::TestRequest) {
        FixMessage hb(msgtype::Heartbeat);
        if (const auto id = msg.getOptionalString(tag::TestReqID)) hb.set(tag::TestReqID, *id);
        sendFix(conn, hb);
        return;
    }
    if (type == msgtype::Heartbeat || type == msgtype::ResendRequest || type == msgtype::SequenceReset) return;
    if (type == msgtype::Logout) {
        closeFix(conn, "bye");
        return;
    }
    if (type == msgtype::SecurityListRequest) {
        const EaStatus s = status();
        FixMessage r(msgtype::SecurityList);
        r.set(tag::SecurityReqID, msg.getOptionalString(tag::SecurityReqID).value_or(""));
        r.set(tag::SecurityResponseID, std::string("mt5-1"));
        r.set(tag::SecurityRequestResult, int64_t{0});
        // The EA's symbol, and plain "XAUUSD" for it too, so a config written
        // for cTrader works on a broker that calls gold "XAUUSDc" or "XAUUSD.m".
        const bool alias = s.symbol != "XAUUSD";
        r.set(tag::NoRelatedSym, static_cast<int64_t>(alias ? 2 : 1));
        r.set(tag::Symbol, std::string(kSymbolId));
        r.set(tag::SymbolName, s.symbol.empty() ? std::string("XAUUSD") : s.symbol);
        r.set(tag::SymbolDigits, static_cast<int64_t>(s.digits));
        if (alias) {
            r.set(tag::Symbol, std::string(kSymbolId));
            r.set(tag::SymbolName, std::string("XAUUSD"));
            r.set(tag::SymbolDigits, static_cast<int64_t>(s.digits));
        }
        sendFix(conn, r);
        return;
    }
    if (type == msgtype::MarketDataRequest) {
        {
            std::scoped_lock lock(mutex_);
            quoteConn_ = conn;
        }
        const EaStatus s = status();
        if (s.bid > 0.0 && s.ask > 0.0) sendQuote(conn, s.bid, s.ask);
        return;
    }
    if (type == msgtype::NewOrderSingle) {
        const std::string id = msg.getOptionalString(tag::ClOrdID).value_or("");
        OrderInfo info;
        info.side = static_cast<int>(msg.getInt(tag::Side));
        info.qty = msg.getDouble(tag::OrderQty);
        info.ordType = static_cast<int>(msg.getInt(tag::OrdType));
        if (msg.has(tag::StopPx)) info.stopPx = msg.getDouble(tag::StopPx);
        {
            std::scoped_lock lock(mutex_);
            orders_[id] = info;
        }
        nlohmann::json cmd;
        cmd["id"] = id;
        cmd["buy"] = info.side == 1;
        cmd["volume"] = std::round(info.qty / kWireQtyPerLot * 1e6) / 1e6;
        if (info.ordType == 3) {
            cmd["c"] = "stop";
            cmd["price"] = info.stopPx;
        } else {
            cmd["c"] = "market";
            cmd["pos"] = msg.getOptionalString(tag::PosMaintRptID).value_or("");
        }
        if (!sendEa(cmd)) rejectOrder(conn, id, "MT5 bridge: MetaTrader 5 isn't connected");
        return;
    }
    if (type == msgtype::OrderCancelRequest) {
        const std::string id = msg.getOptionalString(tag::ClOrdID).value_or("");
        const std::string orig = msg.getOptionalString(tag::OrigClOrdID).value_or("");
        if (!sendEa({{"c", "cancel"}, {"id", id}, {"orig", orig}})) {
            FixMessage r(msgtype::OrderCancelReject);
            r.set(tag::OrderID, std::string("NONE"));
            r.set(tag::ClOrdID, id);
            r.set(tag::OrigClOrdID, orig);
            r.set(tag::OrdStatus, std::string("8"));
            r.set(tag::CxlRejResponseTo, std::string("1"));
            r.set(tag::Text, std::string("MT5 bridge: MetaTrader 5 isn't connected"));
            sendFix(conn, r);
        }
        return;
    }
    if (type == msgtype::RequestForPositions) {
        const std::string req = msg.getOptionalString(tag::PosReqID).value_or("");
        {
            std::scoped_lock lock(mutex_);
            positionReqs_[req] = conn;
        }
        sendEa({{"c", "positions"}, {"req", req}});
        return;
    }
    if (type == msgtype::OrderMassStatusRequest) {
        const std::string req = msg.getOptionalString(tag::MassStatusReqID).value_or("");
        {
            std::scoped_lock lock(mutex_);
            orderReqs_[req] = conn;
        }
        sendEa({{"c", "orders"}, {"req", req}});
        return;
    }
    log("ignored FIX message type " + type + " from Muuk");
}

}  // namespace muuk::mt5_bridge
