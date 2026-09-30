#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "muuk/ctrader_fix/fix_message.hpp"

namespace muuk::mt5_bridge {

// Where the gateway listens. Both on this PC only (127.0.0.1).
struct GatewayConfig {
    std::string bindAddress = "127.0.0.1";
    std::string pipeName = "MuukBridge_5200";  // MuukBridge.mq5 (inside MetaTrader 5) connects to \.\pipe\<this>
    uint16_t fixPort = 5201;  // LiveRunner connects here, both sessions (0 = any free port, for tests)
};

// What the gateway knows about the MT5 side.
struct EaStatus {
    bool connected = false;  // the EA's socket is open
    bool ready = false;      // ...and it said hello (account + symbol known)
    std::string login;
    std::string server;
    std::string company;
    std::string currency;    // "USD", "USC" (cent), ...
    std::string symbol;      // the chart symbol the EA trades, e.g. "XAUUSD" or "XAUUSDc"
    bool hedging = true;     // a netting account can't hold a lock -- the gateway refuses it
    int digits = 2;
    double contractSize = 100.0;
    double volumeMin = 0.01;
    double volumeStep = 0.01;
    double volumeMax = 100.0;
    double leverage = 0.0;
    double hedgedMarginPercent = -1.0;  // <0 = not given
    double tickSize = 0.0;      // price step
    double tickValue = 0.0;     // account money per tick per 1 lot
    double marginPerLot = 0.0;  // account money, at the current price
    bool accountKnown = false;  // an "account" line has arrived (balance/equity below are real)
    double balance = 0.0;
    double equity = 0.0;
    double margin = 0.0;
    double freeMargin = 0.0;
    double bid = 0.0;
    double ask = 0.0;
    int magic = 0;
    int limitOrders = 0;  // the broker's cap on pending orders (0 = none given)
    std::string tradeMode;  // "demo", "contest" or "real"
    double commissionInPerLot = -1.0;   // from the account's own deals; <0 = no history
    double commissionOutPerLot = -1.0;
};

// The MT5 bridge. MuukBridge.mq5 runs inside MetaTrader 5 and talks to this
// gateway over a Windows named pipe in JSON lines: every ~25 ms it sends a
// frame with what happened and reads back the commands waiting for it (a
// pipe needs no MT5 permission; a socket would need 127.0.0.1 in its
// WebRequest list). The gateway in turn speaks the same small
// part of FIX that cTrader does -- Logon, security list, quotes, stop and
// market orders, cancels, execution reports, position and order lists -- so
// LiveRunner connects to 127.0.0.1:fixPort exactly as it would to cTrader,
// and the engine, audit, guards and app work the same on MT5. On top, every
// second an AccountInfo message (msgtype::MuukAccountInfo) carries the
// exact balance, equity, margin, currency and per-position swap from MT5.
//
// EA -> gateway (one JSON object per line, "t" = type):
//   hello {login, server, company, currency, leverage, hedging, symbol, digits,
//          contract, volMin, volStep, volMax, hedgedMargin, magic}
//   tick {bid, ask}
//   account {balance, equity, margin, free, costs: [{pos, swap, comm}]}
//   ack {id, ticket} | reject {id, text} | fill {id, price, volume, pos, buy}
//   cancelled {id} | cancel_reject {id, orig, text}
//   positions {req, list: [{pos, buy, volume, price}]}
//   orders {req, list: [{id, ticket, buy, volume, price}]}
//   log {text}
// gateway -> EA ("c" = command):
//   stop {id, buy, volume, price} | market {id, buy, volume, pos}
//   cancel {id, orig} | positions {req} | orders {req}
class Mt5Gateway {
public:
    using LogHandler = std::function<void(const std::string&)>;

    Mt5Gateway(GatewayConfig config, LogHandler log);
    ~Mt5Gateway();
    Mt5Gateway(const Mt5Gateway&) = delete;
    Mt5Gateway& operator=(const Mt5Gateway&) = delete;

    // Binds both ports and starts accepting. Throws std::runtime_error if a
    // port is taken (e.g. another copy of the app is already the gateway).
    void start();
    void stop();

    [[nodiscard]] const std::string& pipeName() const { return config_.pipeName; }
    [[nodiscard]] uint16_t fixPort() const { return fixPort_; }
    [[nodiscard]] EaStatus status() const;

private:
    struct FixConn {
        explicit FixConn(asio::io_context& io) : socket(io) {}
        asio::ip::tcp::socket socket;
        std::thread reader;
        std::mutex writeMutex;
        int64_t outSeq = 1;
        std::string ourCompId = "MT5";
        std::string theirCompId = "MUUK";
        std::string ourSubId;
        std::string theirSubId;
        std::atomic<bool> open{true};
    };
    using FixConnPtr = std::shared_ptr<FixConn>;

    struct OrderInfo {
        int side = 1;       // 1 buy, 2 sell
        double qty = 0.0;   // wire units (lot x 100)
        int ordType = 3;    // 1 market, 3 stop
        double stopPx = 0.0;
    };

    void eaPipeLoop();
#ifdef _WIN32
    [[nodiscard]] std::wstring pipePath() const;
    [[nodiscard]] void* createPipe() const;  // a HANDLE, nullptr on failure
#endif
    void onEaLine(const std::string& line);
    bool sendEa(const nlohmann::json& cmd);

    void fixAcceptLoop();
    void fixReadLoop(FixConnPtr conn);
    void onFixMessage(const FixConnPtr& conn, const muuk::ctrader_fix::FixMessage& msg);
    void sendFix(const FixConnPtr& conn, const muuk::ctrader_fix::FixMessage& body);
    void closeFix(const FixConnPtr& conn, const std::string& logoutText);
    void closeAllFix(const std::string& logoutText);

    [[nodiscard]] FixConnPtr tradeConn() const;
    [[nodiscard]] FixConnPtr quoteConn() const;
    // Every report carries OrderID(37) -- LiveRunner's parser requires it. orderId "" = "M" + clOrdId.
    [[nodiscard]] muuk::ctrader_fix::FixMessage executionReport(const std::string& clOrdId, const char* execType,
                                                                const char* ordStatus,
                                                                const std::string& orderId = {}) const;
    void sendQuote(const FixConnPtr& conn, double bid, double ask);
    void sendAccountInfo(const FixConnPtr& conn, const nlohmann::json& costs);
    void rejectOrder(const FixConnPtr& conn, const std::string& clOrdId, const std::string& text);

    void log(const std::string& line) const;

    GatewayConfig config_;
    LogHandler log_;
    asio::io_context io_;
    std::unique_ptr<asio::ip::tcp::acceptor> fixAcceptor_;
    uint16_t fixPort_ = 0;
    void* firstPipe_ = nullptr;  // created in start(), handed to the pipe thread
    std::thread eaThread_;
    std::atomic<bool> eaThreadDone_{false};
    std::thread fixAcceptThread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;  // everything below
    EaStatus status_;
    std::vector<std::string> outbox_;  // commands for the EA's next poll
    std::vector<FixConnPtr> fixConns_;
    FixConnPtr tradeConn_;
    FixConnPtr quoteConn_;
    std::map<std::string, OrderInfo> orders_;          // by ClOrdID
    std::map<std::string, FixConnPtr> positionReqs_;   // PosReqID -> who asked
    std::map<std::string, FixConnPtr> orderReqs_;      // MassStatusReqID -> who asked
    nlohmann::json lastCosts_ = nlohmann::json::array();
};

}  // namespace muuk::mt5_bridge
