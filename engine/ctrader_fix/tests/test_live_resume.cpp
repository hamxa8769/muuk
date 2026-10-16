// State persistence: a basket in progress survives the bot being stopped
// and started again (LiveRunner::setStateFile).

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "live_test_helpers.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/live_state.hpp"
#include "muuk/ctrader_fix/messages.hpp"
#include "mock_fix_server.hpp"

using namespace muuk::core;
using namespace muuk::ctrader_fix;
using namespace muuk::ctrader_fix::test;
using namespace std::chrono_literals;

namespace {

struct BrokerPosition {
    std::string id;
    bool buy;
    double lots;
    double price;
    std::string clOrdId = {};  // the order that opened it (the MT5 bridge sends it)
};

struct BrokerOrder {
    std::string clOrdId;
    bool buy;
    double lots;
    double stopPx;
};

LiveConfig resumeConfig(const MockFixServer& server) {
    LiveConfig cfg = testConfig(server);
    cfg.connection.syncPositionsOnConnect = true;
    cfg.strategy.startSide = StartSide::BuyOnly;  // one pending order at a time, like the real setup
    return cfg;
}

std::filesystem::path freshStateFile(const std::string& name) {
    const auto file = std::filesystem::temp_directory_path() / ("muuk_test_" + name + ".state.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return file;
}

// Runs a LiveRunner on its own thread; stops and joins it on destruction.
class Bot {
public:
    Bot(LiveConfig cfg, const std::filesystem::path& stateFile) : runner_(std::move(cfg)) {
        runner_.setStateFile(stateFile);
        thread_ = std::thread([this] {
            try {
                runner_.run();
            } catch (...) {
                error_ = std::current_exception();
            }
        });
    }
    ~Bot() { finish(); }
    Bot(const Bot&) = delete;
    Bot& operator=(const Bot&) = delete;

    LiveRunner& runner() { return runner_; }
    void finish() {
        runner_.stop();
        if (thread_.joinable()) thread_.join();
    }
    void rethrow() {
        if (error_) std::rethrow_exception(error_);
    }

private:
    LiveRunner runner_;
    std::thread thread_;
    std::exception_ptr error_;
};

// Logon, symbol list, quote subscription and the connect-time sync, with
// the broker holding `positions` and working `orders`. Returns the next
// free trade-session sequence number.
int handshake(MockFixServer& server, const std::vector<BrokerPosition>& positions,
              const std::vector<BrokerOrder>& orders) {
    (void)server.waitForMessage(msgtype::Logon, kTradeConn);
    server.sendToClient(kTradeConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::Logon, kQuoteConn);
    server.sendToClient(kQuoteConn, serverHeaderMessage(msgtype::Logon, 1));
    (void)server.waitForMessage(msgtype::SecurityListRequest, kTradeConn);
    server.sendToClient(kTradeConn, buildSecurityListResponse());
    (void)server.waitForMessage(msgtype::MarketDataRequest, kQuoteConn);

    int seq = 3;
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 3s);
    if (positions.empty()) {
        FixMessage none = serverHeaderMessage(msgtype::PositionReport, seq++);
        none.set(tag::PosReqID, posReq.getString(tag::PosReqID));
        none.set(tag::PosReqResult, std::string("2"));
        server.sendToClient(kTradeConn, none);
    }
    for (const auto& p : positions) {
        FixMessage r = serverHeaderMessage(msgtype::PositionReport, seq++);
        r.set(tag::PosReqID, posReq.getString(tag::PosReqID));
        r.set(tag::PosMaintRptID, p.id);
        r.set(tag::TotalNumPosReports, static_cast<int64_t>(positions.size()));
        r.set(tag::PosReqResult, std::string("0"));
        r.set(tag::Symbol, std::string("1"));
        r.set(tag::NoPositions, int64_t{1});
        r.set(tag::LongQty, p.buy ? p.lots * 100.0 : 0.0, 2);
        r.set(tag::ShortQty, p.buy ? 0.0 : p.lots * 100.0, 2);
        r.set(tag::SettlPrice, p.price, 2);
        server.sendToClient(kTradeConn, r);
    }

    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 3s);
    REQUIRE(ordReq.getInt(tag::MassStatusReqType) == 7);
    if (orders.empty()) {
        // Exactly what the live IC Markets server sends: no RefMsgType, no ref id.
        FixMessage none = serverHeaderMessage(msgtype::BusinessMessageReject, seq++);
        none.set(tag::BusinessRejectReason, int64_t{0});
        none.set(tag::Text, std::string("ORDER_NOT_FOUND:no orders found"));
        server.sendToClient(kTradeConn, none);
    }
    for (const auto& o : orders) {
        FixMessage r = serverHeaderMessage(msgtype::ExecutionReport, seq++);
        r.set(tag::OrderID, "B" + o.clOrdId);
        r.set(tag::ClOrdID, o.clOrdId);
        r.set(tag::MassStatusReqID, ordReq.getString(tag::MassStatusReqID));
        r.set(tag::TotNumReports, static_cast<int64_t>(orders.size()));
        r.set(tag::ExecType, std::string("I"));
        r.set(tag::OrdStatus, std::string("0"));
        r.set(tag::Symbol, std::string("1"));
        r.set(tag::Side, static_cast<int64_t>(o.buy ? 1 : 2));
        r.set(tag::OrderQty, o.lots * 100.0, 2);
        r.set(tag::OrdType, int64_t{3});
        r.set(tag::StopPx, o.stopPx, 2);
        server.sendToClient(kTradeConn, r);
    }
    return seq;
}

void sendQuote(MockFixServer& server, int seq, double bid, double ask) {
    FixMessage tick = serverHeaderMessage(msgtype::MarketDataSnapshotFullRefresh, seq);
    tick.set(tag::Symbol, std::string("1"));
    tick.set(tag::NoMDEntries, int64_t{2});
    tick.set(tag::MDEntryType, std::string("0"));
    tick.set(tag::MDEntryPx, bid, 2);
    tick.set(tag::MDEntryType, std::string("1"));
    tick.set(tag::MDEntryPx, ask, 2);
    server.sendToClient(kQuoteConn, tick);
}

void sendFill(MockFixServer& server, int seq, const std::string& clOrdId, double price, double lots,
              const std::string& positionId) {
    FixMessage fill = serverHeaderMessage(msgtype::ExecutionReport, seq);
    fill.set(tag::OrderID, "B" + clOrdId);
    fill.set(tag::ClOrdID, clOrdId);
    if (!positionId.empty()) fill.set(tag::PosMaintRptID, positionId);
    fill.set(tag::ExecType, std::string("F"));
    fill.set(tag::OrdStatus, std::string("2"));
    fill.set(tag::AvgPx, price, 2);
    fill.set(tag::LastQty, lots, 2);
    fill.set(tag::CumQty, lots, 2);
    fill.set(tag::LeavesQty, std::string("0"));
    server.sendToClient(kTradeConn, fill);
}

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

struct FirstRun {
    std::string buyId;   // filled: position P1, BUY 0.01 @ 4337.00
    std::string sellId;  // still pending: Sell Stop 0.02 @ 4336.00
};

// Session 1: the range opens with a Buy Stop, it fills, the Sell Stop goes
// out -- then the bot is stopped (app closed / connection lost).
FirstRun runFirstSession(const std::filesystem::path& stateFile) {
    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    int seq = handshake(server, {}, {});

    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(buy.getInt(tag::Side) == 1);
    REQUIRE(buy.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(sell.getInt(tag::Side) == 2);
    REQUIRE(sell.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(sell.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().pendingOrders.size() == 1; }));

    bot.finish();
    bot.rethrow();

    // What was saved: the running basket, exactly.
    const auto saved = loadState(stateFile);
    REQUIRE(saved.has_value());
    REQUIRE(saved->engine.status == EngineStatus::Active);
    REQUIRE(saved->openLegs.size() == 1);
    REQUIRE(saved->openLegs[0].positionId == "P1");
    REQUIRE(saved->pendingOrders.size() == 1);
    REQUIRE(std::to_string(saved->pendingOrders[0].id) == sell.getString(tag::ClOrdID));
    REQUIRE(saved->currentBasket.has_value());
    REQUIRE(saved->currentBasket->legs.size() == 1);
    return FirstRun{buy.getString(tag::ClOrdID), sell.getString(tag::ClOrdID)};
}

}  // namespace

TEST_CASE("Resume: a basket still intact at the broker simply carries on after a restart") {
    const auto stateFile = freshStateFile("resume_intact");
    const FirstRun first = runFirstSession(stateFile);

    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    int seq = handshake(server, {{"P1", true, 0.01, 4337.00}}, {{first.sellId, false, 0.02, 4336.00}});
    sendQuote(server, 2, 4336.50, 4336.60);

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().connected; }));
    // Nothing new is placed: the basket is already fully in place.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 500ms), std::runtime_error);
    LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.positions.size() == 1);
    REQUIRE_FALSE(snap.positions[0].adopted);
    REQUIRE(snap.positions[0].positionId == "P1");
    REQUIRE(snap.pendingOrders.size() == 1);
    REQUIRE_FALSE(snap.pendingOrders[0].adopted);
    REQUIRE(snap.resumeNote.find("Resumed basket #1") != std::string::npos);
    REQUIRE(snap.currentBasket.has_value());
    REQUIRE(snap.currentBasket->number == 1);

    // The Sell Stop fills: the sequence continues where it was -- next is a Buy Stop of 0.03.
    sendFill(server, seq++, first.sellId, 4336.00, 2.0, "P2");
    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(next.getInt(tag::Side) == 1);
    REQUIRE(next.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(next.getDouble(tag::OrderQty) == Catch::Approx(3.0));
    REQUIRE(std::stoull(next.getString(tag::ClOrdID)) > std::stoull(first.sellId));  // ids never reused

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Resume: a pending order that filled while disconnected is processed as that fill") {
    const auto stateFile = freshStateFile("resume_filled");
    const FirstRun first = runFirstSession(stateFile);

    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    // The Sell Stop is gone from the working orders, and a SELL 0.02 opened near its price.
    (void)handshake(server, {{"P1", true, 0.01, 4337.00}, {"P2", false, 0.02, 4335.97}}, {});
    sendQuote(server, 2, 4335.90, 4336.00);

    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(next.getInt(tag::Side) == 1);
    REQUIRE(next.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(next.getDouble(tag::OrderQty) == Catch::Approx(3.0));

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 2; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    for (const auto& p : snap.positions) REQUIRE_FALSE(p.adopted);
    REQUIRE(snap.positions[1].positionId == "P2");
    REQUIRE(snap.positions[1].entryPrice == Catch::Approx(4335.97));
    REQUIRE(snap.resumeNote.find("while away") != std::string::npos);
    REQUIRE(snap.currentBasket->legs.size() == 2);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Resume: refused when a position was closed outside the bot -- orders cancelled, nothing new opens") {
    const auto stateFile = freshStateFile("resume_broken");
    const FirstRun first = runFirstSession(stateFile);

    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    // P1 is gone (closed by hand in cTrader); the Sell Stop is still working.
    (void)handshake(server, {}, {{first.sellId, false, 0.02, 4336.00}});
    sendQuote(server, 2, 4336.50, 4336.60);

    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == first.sellId);
    // autoContinue is on, but after an interrupted basket nothing opens by itself.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 600ms), std::runtime_error);

    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Idle);
    REQUIRE(snap.positions.empty());
    REQUIRE(snap.resumeNote.find("Could not safely resume") != std::string::npos);
    // The interrupted basket is recorded, with the position marked as closed outside the bot.
    REQUIRE(snap.baskets.size() == 1);
    REQUIRE(snap.baskets[0].closeReason.rfind("interrupted", 0) == 0);
    REQUIRE(snap.baskets[0].legs.size() == 1);
    REQUIRE(snap.baskets[0].legs[0].note.find("outside the bot") != std::string::npos);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Resume: a basket close that never went through is redone on reconnect") {
    const auto stateFile = freshStateFile("resume_closing");
    std::string firstCloseId;
    {
        MockFixServer server;
        LiveConfig cfg = resumeConfig(server);
        cfg.strategy.basketProfitTargetMoney = 1.0;
        Bot bot(cfg, stateFile);
        int seq = handshake(server, {}, {});
        const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
        sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
        (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // the Sell Stop
        sendQuote(server, 2, 4338.60, 4338.70);  // +$1.6 gross on 0.01 lot: target reached
        const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
        REQUIRE(close.getInt(tag::OrdType) == 1);
        firstCloseId = close.getString(tag::ClOrdID);
        (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);  // the Sell Stop
        // ...and the connection drops before the close is confirmed.
        bot.finish();
        bot.rethrow();
    }
    const auto saved = loadState(stateFile);
    REQUIRE(saved->engine.status == EngineStatus::Completed);
    REQUIRE(saved->pendingCloses.size() == 1);

    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.strategy.basketProfitTargetMoney = 1.0;
    cfg.connection.autoContinue = false;
    Bot bot(cfg, stateFile);
    int seq = handshake(server, {{"P1", true, 0.01, 4337.00}}, {});  // still open: the close never happened
    sendQuote(server, 2, 4338.60, 4338.70);

    const FixMessage reclose = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(reclose.getInt(tag::OrdType) == 1);
    REQUIRE(reclose.getInt(tag::Side) == 2);
    REQUIRE(reclose.getString(tag::PosMaintRptID) == "P1");
    REQUIRE(reclose.getString(tag::ClOrdID) != firstCloseId);  // a fresh ClOrdID, never a reused one

    sendFill(server, seq++, reclose.getString(tag::ClOrdID), 4338.60, 1.0, "P1");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.baskets[0].closeReason == "profit target");
    REQUIRE(snap.baskets[0].net == Catch::Approx(1.60));
    REQUIRE(snap.positions.empty());

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lifetime statistics carry over a restart and resetStats() starts them over") {
    const auto stateFile = freshStateFile("stats");
    const FirstRun first = runFirstSession(stateFile);
    (void)first;
    const auto saved = loadState(stateFile);
    REQUIRE(saved->fillReactionUs.count == 1);  // one fill reacted to in the first run
    REQUIRE_FALSE(saved->statsSince.empty());

    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    Bot bot(cfg, stateFile);
    (void)handshake(server, {{"P1", true, 0.01, 4337.00}}, {{first.sellId, false, 0.02, 4336.00}});
    sendQuote(server, 2, 4336.50, 4336.60);
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().connected; }));
    LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.fillReactionUs.samples == 1);  // carried over
    REQUIRE(snap.statsSince == saved->statsSince);

    bot.runner().resetStats();
    snap = bot.runner().snapshot();
    REQUIRE(snap.fillReactionUs.samples == 0);
    REQUIRE(snap.ticksProcessed == 0);
    REQUIRE(snap.status == EngineStatus::Active);  // the basket itself is untouched
    REQUIRE(snap.positions.size() == 1);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("The state file round-trips through JSON exactly") {
    PersistedState s;
    s.account = "demo.123";
    s.symbol = "XAUUSD";
    s.engine.status = EngineStatus::Active;
    s.engine.range = RangeConfig{Price{433600}, Price{433700}};
    EngineState::Armed armed;
    armed.order = PendingOrder{7, 2, OrderSide::SellStop, Price{433600}, Volume{2}, OrderState::Pending};
    armed.retryCount = 1;
    s.engine.pendingOrders.push_back(armed);
    s.engine.buyVolumeSteps = 1;
    s.engine.buySumEntryPriceTicksVolume = 433700;
    s.engine.nextSequenceIndex = 3;
    s.engine.nextOrderId = 8;
    s.engine.slippageMoneyAccrued = 0.02;
    s.openLegs.push_back(PersistedLeg{6, true, 1, 433702, 433700, "P1", false, ""});
    s.pendingCloses.push_back(PersistedLeg{5, false, 3, 433500, 433500, "P0", true, "x9"});
    s.basketCounter = 4;
    s.nextCloseReqId = 10;
    s.tickDecisionUs = LatencyTracker::Totals{100, 250.0, 9.5};
    BasketRecord b;
    b.number = 4;
    b.legs.push_back(BasketLegRecord{});
    s.currentBasket = b;

    const PersistedState r = stateFromJson(stateToJson(s));
    REQUIRE(r.engine.status == EngineStatus::Active);
    REQUIRE(r.engine.range.upperBound == Price{433700});
    REQUIRE(r.engine.pendingOrders.size() == 1);
    REQUIRE(r.engine.pendingOrders[0].order.id == 7);
    REQUIRE(r.engine.pendingOrders[0].order.side == OrderSide::SellStop);
    REQUIRE(r.engine.pendingOrders[0].order.sequenceIndex == 2);
    REQUIRE(r.engine.pendingOrders[0].retryCount == 1);
    REQUIRE(r.engine.buySumEntryPriceTicksVolume == 433700);
    REQUIRE(r.engine.nextOrderId == 8);
    REQUIRE(r.engine.slippageMoneyAccrued == Catch::Approx(0.02));
    REQUIRE(r.openLegs.size() == 1);
    REQUIRE(r.openLegs[0].priceTicks == 433702);
    REQUIRE(r.pendingCloses[0].closeClOrdId == "x9");
    REQUIRE(r.pendingCloses[0].adopted);
    REQUIRE(r.basketCounter == 4);
    REQUIRE(r.nextCloseReqId == 10);
    REQUIRE(r.tickDecisionUs.count == 100);
    REQUIRE(r.currentBasket.has_value());
    REQUIRE(r.currentBasket->legs.size() == 1);

    nlohmann::json newer = stateToJson(s);
    newer["version"] = PersistedState::kVersion + 1;
    REQUIRE_THROWS(stateFromJson(newer));
}

TEST_CASE("A basket found at the broker with no saved state is picked up and its missing next order placed") {
    const auto stateFile = freshStateFile("adopt_basket");
    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);  // range width $1.00, lots 0.01 + 0.01, Buy first
    // BUY 0.01 anchored the range at 4337.00, then SELL 0.02 and BUY 0.03 -- and no pending order.
    (void)handshake(server, {{"P1", true, 0.01, 4337.00}, {"P2", false, 0.02, 4335.98}, {"P3", true, 0.03, 4337.02}},
                    {});
    sendQuote(server, 2, 4336.50, 4336.60);

    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(next.getInt(tag::OrdType) == 3);
    REQUIRE(next.getInt(tag::Side) == 2);
    REQUIRE(next.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(next.getDouble(tag::OrderQty) == Catch::Approx(4.0));  // 0.04 lot

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().pendingOrders.size() == 1; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.positions.size() == 3);
    for (const auto& p : snap.positions) REQUIRE_FALSE(p.adopted);
    REQUIRE(snap.rangeLower == Catch::Approx(4336.00));
    REQUIRE(snap.rangeUpper == Catch::Approx(4337.00));
    REQUIRE(snap.resumeNote.find("Picked up the basket") != std::string::npos);
    REQUIRE(snap.currentBasket.has_value());
    REQUIRE(snap.currentBasket->legs.size() == 3);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("A next order the market has already passed waits, and goes out once the price is back") {
    const auto stateFile = freshStateFile("waiting_order");
    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    (void)handshake(server, {{"P1", true, 0.01, 4337.00}, {"P2", false, 0.02, 4336.00}, {"P3", true, 0.03, 4337.00}},
                    {});
    sendQuote(server, 2, 4330.00, 4330.10);  // already far below the Sell Stop's 4336.00

    // Not sent: the broker would refuse a Sell Stop above the bid.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 600ms), std::runtime_error);
    LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.pendingOrders.size() == 1);
    REQUIRE(snap.pendingOrders[0].waiting);
    REQUIRE(snap.pendingOrders[0].price == Catch::Approx(4336.00));
    REQUIRE(snap.resumeNote.find("waits for the price") != std::string::npos);

    sendQuote(server, 3, 4336.40, 4336.50);  // back above it
    const FixMessage placed = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(placed.getInt(tag::OrdType) == 3);
    REQUIRE(placed.getInt(tag::Side) == 2);
    REQUIRE(placed.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(waitUntil([&] {
        const auto s = bot.runner().snapshot();
        return s.pendingOrders.size() == 1 && !s.pendingOrders[0].waiting;
    }));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("placeWaitingOrderNow sends a waiting order at market, and its fill carries the basket on") {
    const auto stateFile = freshStateFile("waiting_now");
    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    int seq = handshake(server,
                        {{"P1", true, 0.01, 4337.00}, {"P2", false, 0.02, 4336.00}, {"P3", true, 0.03, 4337.00}}, {});
    sendQuote(server, 2, 4330.00, 4330.10);
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().pendingOrders.size() == 1; }));
    const OrderId waitingId = bot.runner().snapshot().pendingOrders[0].id;

    bot.runner().placeWaitingOrderNow(waitingId);
    const FixMessage market = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(market.getInt(tag::OrdType) == 1);
    REQUIRE(market.getInt(tag::Side) == 2);
    REQUIRE(market.getDouble(tag::OrderQty) == Catch::Approx(4.0));
    REQUIRE_FALSE(market.has(tag::PosMaintRptID));  // opens a new position
    REQUIRE(market.getString(tag::ClOrdID) == std::to_string(waitingId));

    sendFill(server, seq++, market.getString(tag::ClOrdID), 4330.00, 4.0, "P4");
    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(next.getInt(tag::Side) == 1);
    REQUIRE(next.getDouble(tag::StopPx) == Catch::Approx(4337.00));
    REQUIRE(next.getDouble(tag::OrderQty) == Catch::Approx(5.0));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 4; }));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Resume: a pending order that vanished from the broker without a fill is placed again") {
    const auto stateFile = freshStateFile("resume_replace");
    const FirstRun first = runFirstSession(stateFile);

    MockFixServer server;
    Bot bot(resumeConfig(server), stateFile);
    (void)handshake(server, {{"P1", true, 0.01, 4337.00}}, {});  // the Sell Stop is gone, nothing filled
    sendQuote(server, 2, 4336.50, 4336.60);

    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::Side) == 2);
    REQUIRE(again.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(again.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(again.getString(tag::ClOrdID) != first.sellId);  // a new ClOrdID
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Active);
    REQUIRE(snap.resumeNote.find("Resumed") != std::string::npos);

    bot.finish();
    bot.rethrow();
}

namespace {
void sendOrderEvent(MockFixServer& server, int seq, const std::string& clOrdId, const char* execType,
                    const char* ordStatus) {
    FixMessage er = serverHeaderMessage(msgtype::ExecutionReport, seq);
    er.set(tag::OrderID, "B" + clOrdId);
    er.set(tag::ClOrdID, clOrdId);
    er.set(tag::ExecType, std::string(execType));
    er.set(tag::OrdStatus, std::string(ordStatus));
    er.set(tag::Text, std::string("test"));
    server.sendToClient(kTradeConn, er);
}
}  // namespace

TEST_CASE("A pending order cancelled outside the bot (broker or cTrader) is placed again") {
    MockFixServer server;
    Bot bot(resumeConfig(server), freshStateFile("outside_cancel"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);

    sendOrderEvent(server, seq++, sell.getString(tag::ClOrdID), "4", "4");  // cancelled -- not by the bot
    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::Side) == 2);
    REQUIRE(again.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(again.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(again.getString(tag::ClOrdID) != sell.getString(tag::ClOrdID));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Watchdog: a running basket left without any pending order gets its next one") {
    MockFixServer server;
    Bot bot(resumeConfig(server), freshStateFile("watchdog"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");

    // The broker rejects the Sell Stop and all 3 retries: the engine gives up on it.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
        REQUIRE(sell.getInt(tag::Side) == 2);
        sendOrderEvent(server, seq++, sell.getString(tag::ClOrdID), "8", "8");
    }
    // Within a couple of seconds the watchdog notices and places it again.
    const FixMessage rearmed = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(rearmed.getInt(tag::Side) == 2);
    REQUIRE(rearmed.getDouble(tag::StopPx) == Catch::Approx(4336.00));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Close range: after the manual-close cooldown a new range opens by itself (not when shutting down)") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.manualCloseCooldownSeconds = 1;
    Bot bot(cfg, freshStateFile("manual_reopen"));
    int seq = handshake(server, {}, {});
    sendQuote(server, 2, 4336.50, 4336.60);

    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // the Sell Stop

    bot.runner().closeRange();
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getInt(tag::OrdType) == 1);
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4336.50, 1.0, "P1");

    // Flat: the countdown shows, nothing is placed yet...
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().cooldownRemainingSeconds > 0; }));
    REQUIRE(bot.runner().snapshot().status == EngineStatus::Idle);
    // ...and then a new range opens around the live price.
    const FixMessage reopened = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(reopened.getInt(tag::OrdType) == 3);
    REQUIRE(reopened.getInt(tag::Side) == 1);
    REQUIRE(reopened.getDouble(tag::StopPx) == Catch::Approx(4337.05));  // mid 4336.55 + half of $1.00
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().status == EngineStatus::Active; }));

    // Closing for shutdown (exit guard) never reopens.
    bot.runner().closeRange(false);
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 2500ms), std::runtime_error);
    REQUIRE(bot.runner().snapshot().status == EngineStatus::Idle);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("An order the broker never confirms is checked with the broker and placed again") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.orderAckTimeoutSeconds = 1;
    Bot bot(cfg, freshStateFile("ack_timeout"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage lost = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // never answered

    // After the timeout the bot asks which orders the broker has...
    const FixMessage check = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 5s);
    FixMessage none = serverHeaderMessage(msgtype::BusinessMessageReject, seq++);
    none.set(tag::BusinessRejectReason, int64_t{0});
    none.set(tag::Text, std::string("ORDER_NOT_FOUND:no orders found"));
    server.sendToClient(kTradeConn, none);

    // ...and, since it isn't there, places it again.
    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::Side) == 2);
    REQUIRE(again.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(again.getDouble(tag::OrderQty) == Catch::Approx(2.0));
    REQUIRE(again.getString(tag::ClOrdID) != lost.getString(tag::ClOrdID));
    (void)check;

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Stop loss: the basket is closed and recorded as a stop loss") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.strategy.basketProfitTargetMoney = 50.0;
    cfg.strategy.basketStopLossMoney = 1.0;
    Bot bot(cfg, freshStateFile("stop_loss"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell Stop @ 4336
    sendQuote(server, 2, 4335.80, 4335.90);  // -$1.15 net on the open BUY
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getInt(tag::OrdType) == 1);
    REQUIRE(close.getString(tag::PosMaintRptID) == "P1");
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4335.80, 1.0, "P1");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }));
    REQUIRE(bot.runner().snapshot().baskets[0].closeReason == "stop loss");

    bot.finish();
    bot.rethrow();
}

namespace {
// Answers a Request For Positions / Order Mass Status Request the way the
// live IC Markets server does. Returns the next sequence number.
int answerPositions(MockFixServer& server, int seq, const FixMessage& request,
                    const std::vector<BrokerPosition>& positions) {
    if (positions.empty()) {
        FixMessage none = serverHeaderMessage(msgtype::PositionReport, seq++);
        none.set(tag::PosReqID, request.getString(tag::PosReqID));
        none.set(tag::PosReqResult, std::string("2"));
        server.sendToClient(kTradeConn, none);
    }
    for (const auto& p : positions) {
        FixMessage r = serverHeaderMessage(msgtype::PositionReport, seq++);
        r.set(tag::PosReqID, request.getString(tag::PosReqID));
        r.set(tag::PosMaintRptID, p.id);
        r.set(tag::TotalNumPosReports, static_cast<int64_t>(positions.size()));
        r.set(tag::PosReqResult, std::string("0"));
        r.set(tag::Symbol, std::string("1"));
        r.set(tag::LongQty, p.buy ? p.lots * 100.0 : 0.0, 2);
        r.set(tag::ShortQty, p.buy ? 0.0 : p.lots * 100.0, 2);
        r.set(tag::SettlPrice, p.price, 2);
        if (!p.clOrdId.empty()) r.set(tag::ClOrdID, p.clOrdId);
        server.sendToClient(kTradeConn, r);
    }
    return seq;
}

int answerOrders(MockFixServer& server, int seq, const FixMessage& request, const std::vector<BrokerOrder>& orders) {
    if (orders.empty()) {
        FixMessage none = serverHeaderMessage(msgtype::BusinessMessageReject, seq++);
        none.set(tag::BusinessRejectReason, int64_t{0});
        none.set(tag::Text, std::string("ORDER_NOT_FOUND:no orders found"));
        server.sendToClient(kTradeConn, none);
    }
    for (const auto& o : orders) {
        FixMessage r = serverHeaderMessage(msgtype::ExecutionReport, seq++);
        r.set(tag::OrderID, "B" + o.clOrdId);
        r.set(tag::ClOrdID, o.clOrdId);
        r.set(tag::MassStatusReqID, request.getString(tag::MassStatusReqID));
        r.set(tag::TotNumReports, static_cast<int64_t>(orders.size()));
        r.set(tag::ExecType, std::string("I"));
        r.set(tag::OrdStatus, std::string("0"));
        r.set(tag::Symbol, std::string("1"));
        r.set(tag::Side, static_cast<int64_t>(o.buy ? 1 : 2));
        r.set(tag::OrderQty, o.lots * 100.0, 2);
        r.set(tag::OrdType, int64_t{3});
        r.set(tag::StopPx, o.stopPx, 2);
        server.sendToClient(kTradeConn, r);
    }
    return seq;
}

void sendNewAck(MockFixServer& server, int seq, const std::string& clOrdId) {
    FixMessage ack = serverHeaderMessage(msgtype::ExecutionReport, seq);
    ack.set(tag::OrderID, "B" + clOrdId);
    ack.set(tag::ClOrdID, clOrdId);
    ack.set(tag::ExecType, std::string("0"));
    ack.set(tag::OrdStatus, std::string("0"));
    server.sendToClient(kTradeConn, ack);
}
}  // namespace

TEST_CASE("Order ids are unique per connect (time-based), so another copy's reports are never taken as ours") {
    MockFixServer server;
    Bot bot(resumeConfig(server), freshStateFile("unique_ids"));
    (void)handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(std::stoull(buy.getString(tag::ClOrdID)) >= 1'000'000'000'000ULL);

    // A fill for an id this bot never used (another copy's order "7") is ignored.
    sendFill(server, 3, "7", 4337.00, 1.0, "P-OTHER");
    std::this_thread::sleep_for(300ms);
    REQUIRE(bot.runner().snapshot().positions.empty());

    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.accountLogin == "12345");
    REQUIRE(snap.brokerUid == "testbroker");
    REQUIRE(snap.environment == "demo");

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker check: an order the broker does not have is placed again, and the check is shown") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.verifySeconds = 1;
    Bot bot(cfg, freshStateFile("verify_missing"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, buy.getString(tag::ClOrdID));
    REQUIRE(waitUntil([&] {
        const auto s = bot.runner().snapshot();
        return !s.pendingOrders.empty() && s.pendingOrders[0].confirmed;
    }));

    // The periodic check: the broker has no positions and -- wrongly -- no orders.
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {});
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 4s);
    seq = answerOrders(server, seq, ordReq, {});

    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::Side) == 1);
    REQUIRE(again.getDouble(tag::StopPx) == Catch::Approx(buy.getDouble(tag::StopPx)));
    REQUIRE(again.getString(tag::ClOrdID) != buy.getString(tag::ClOrdID));
    REQUIRE(waitUntil([&] { return !bot.runner().snapshot().brokerCheckedAt.empty(); }));
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.brokerIssuesRecent >= 1);
    REQUIRE(snap.brokerIssues.back().find("MISSED") != std::string::npos);
    REQUIRE(snap.currentBasket->ordersMissed == 1);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker check: a fill whose report was lost (MT5 offline) is booked as the basket's own, not sent again") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.verifySeconds = 1;
    Bot bot(cfg, freshStateFile("lost_fill"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(sell.getInt(tag::Side) == 2);
    sendNewAck(server, seq++, sell.getString(tag::ClOrdID));

    // The Sell Stop filled while MT5 was offline: no fill report ever comes,
    // the broker just has a second position, opened by that order.
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq,
                          {{"P1", true, 0.01, 4337.00}, {"P2", false, 0.02, 4335.90, sell.getString(tag::ClOrdID)}});
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 4s);
    seq = answerOrders(server, seq, ordReq, {});

    // The next level goes out (a Buy Stop), not the Sell Stop again.
    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(next.getInt(tag::Side) == 1);
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 2; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    for (const auto& p : snap.positions) REQUIRE_FALSE(p.adopted);
    REQUIRE(snap.anotherTrader.empty());
    REQUIRE(snap.currentBasket->ordersFilled == 2);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker check: a position nobody of this bot opened warns of another trader and stops new ranges") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.verifySeconds = 1;
    Bot bot(cfg, freshStateFile("another_trader"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, buy.getString(tag::ClOrdID));

    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {{"P9", false, 0.08, 4335.00, "1792043083999"}});
    REQUIRE(waitUntil([&] { return !bot.runner().snapshot().anotherTrader.empty(); }));
    REQUIRE(bot.runner().snapshot().anotherTrader.find("P9") != std::string::npos);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker check: a basket position closed outside the bot stops the basket safely") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.verifySeconds = 1;
    Bot bot(cfg, freshStateFile("verify_gone"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sell = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sell.getString(tag::ClOrdID));

    // P1 was closed by hand in cTrader; the Sell Stop is still working.
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {});
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 4s);
    seq = answerOrders(server, seq, ordReq, {{sell.getString(tag::ClOrdID), false, 0.02, 4336.00}});

    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == sell.getString(tag::ClOrdID));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    REQUIRE(snap.status == EngineStatus::Idle);
    REQUIRE(snap.positions.empty());
    REQUIRE(snap.baskets[0].outcome == "interrupted");
    REQUIRE(snap.baskets[0].legs[0].note == "closed outside the bot");
    // Nothing new opens by itself after that.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms), std::runtime_error);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker check: a working order this bot did not place is listed and reported") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.verifySeconds = 1;
    Bot bot(cfg, freshStateFile("verify_foreign"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, buy.getString(tag::ClOrdID));

    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {});
    const FixMessage ordReq = server.waitForMessage(msgtype::OrderMassStatusRequest, kTradeConn, 4s);
    seq = answerOrders(server, seq, ordReq,
                       {{buy.getString(tag::ClOrdID), true, 0.01, buy.getDouble(tag::StopPx)},
                        {"555", false, 0.50, 4300.00}});  // someone else's

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().pendingOrders.size() == 2; }));
    const LiveSnapshot snap = bot.runner().snapshot();
    const auto foreign = std::find_if(snap.pendingOrders.begin(), snap.pendingOrders.end(),
                                      [](const PendingOrderInfo& o) { return o.id == 555; });
    REQUIRE(foreign != snap.pendingOrders.end());
    REQUIRE(foreign->adopted);
    REQUIRE(snap.brokerIssues.back().find("didn't place") != std::string::npos);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Each basket records how it ended and how it went: orders, fills, deepest drawdown") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.strategy.basketProfitTargetMoney = 0.50;
    Bot bot(cfg, freshStateFile("measure"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell Stop
    sendQuote(server, 2, 4336.60, 4336.70);                                 // the low point
    sendQuote(server, 3, 4337.60, 4337.70);                                 // target
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getInt(tag::OrdType) == 1);
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4337.60, 1.0, "P1");

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }));
    const BasketRecord b = bot.runner().snapshot().baskets[0];
    REQUIRE(b.outcome == "target");
    REQUIRE(b.ordersPlaced == 2);
    REQUIRE(b.ordersFilled == 1);
    REQUIRE(b.ordersMissed == 0);
    REQUIRE(b.worstNet == Catch::Approx(-0.40));  // mid 4336.65 vs 4337 = -0.35, minus half the spread
    REQUIRE(b.maxOpenLots == Catch::Approx(0.01));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Broker-exact commission: the target waits for it, and the booked net is what the broker credits") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.strategy.basketProfitTargetMoney = 0.50;
    cfg.strategy.costs.commissionPerLotRoundTurn = 7.0;     // ignored: the per-million rate wins
    cfg.strategy.costs.commissionPerMillionPerSide = 30.0;  // IC Markets cTrader Raw
    Bot bot(cfg, freshStateFile("broker_commission"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell Stop

    // 0.01 lot x 100 oz x 4337.00 x 30 / 1M = 0.13 a side -> 0.26 open + close (a flat $7/lot would say 0.07).
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 1; }));
    sendQuote(server, 2, 4337.60, 4337.70);  // gross 0.60 - 0.26 = 0.34: not there yet
    REQUIRE(waitUntil([&] {
        const auto s = bot.runner().snapshot();
        return !s.positions.empty() && s.bid && *s.bid == Catch::Approx(4337.60);
    }));
    const LiveSnapshot mid = bot.runner().snapshot();
    REQUIRE(mid.positions[0].pnl.commission == Catch::Approx(0.26));
    REQUIRE(mid.positions[0].pnl.net == Catch::Approx(0.34));
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 800ms), std::runtime_error);

    sendQuote(server, 3, 4337.90, 4338.00);  // 0.90 - 0.26 = 0.64: target
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getInt(tag::OrdType) == 1);
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4337.85, 1.0, "P1");  // filled 5 cents lower

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }));
    const BasketRecord b = bot.runner().snapshot().baskets[0];
    REQUIRE(b.outcome == "target");
    REQUIRE(b.netAtTrigger == Catch::Approx(0.64));
    REQUIRE(b.legs[0].commission == Catch::Approx(0.26));
    REQUIRE(b.gross == Catch::Approx(0.85));
    REQUIRE(b.net == Catch::Approx(0.59));  // = the balance change at the broker
    REQUIRE(bot.runner().snapshot().realizedPnl == Catch::Approx(0.59));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Market hours: no new range during a pause (here a news guard); it opens once the pause is gone") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.market.enabled = true;
    NewsEvent nfp;
    nfp.utcMs = utcNowMs();  // right now -- whatever the clock, the market is paused (news, break or weekend)
    nfp.title = "NFP";
    nfp.manual = true;
    cfg.market.news.push_back(nfp);
    Bot bot(cfg, freshStateFile("market_pause"));
    (void)handshake(server, {}, {});
    sendQuote(server, 2, 4336.50, 4336.60);

    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms), std::runtime_error);
    const LiveSnapshot paused = bot.runner().snapshot();
    REQUIRE(paused.marketKnown);
    REQUIRE_FALSE(paused.market.canOpenRange);
    REQUIRE(paused.status == EngineStatus::Idle);
    REQUIRE_FALSE(bot.runner().openRange());  // by hand too

    LiveSettings s = bot.runner().settings();
    s.market.enabled = false;  // filter off
    bot.runner().applySettings(s);
    const FixMessage first = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(first.getInt(tag::Side) == 1);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Entry timeout: an entry order the price walked away from is cancelled and the range opens again around it") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.entryTimeoutSeconds = 1;
    Bot bot(cfg, freshStateFile("entry_timeout"));
    (void)handshake(server, {}, {});
    const FixMessage first = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    const double firstStop = first.getDouble(tag::StopPx);

    // Nothing fills; the price is far above the old entry a little later.
    std::this_thread::sleep_for(1200ms);
    sendQuote(server, 2, 4345.00, 4345.10);
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == first.getString(tag::ClOrdID));
    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::Side) == 1);  // Buy Stop first, as configured
    REQUIRE(again.getDouble(tag::StopPx) > 4345.0);
    REQUIRE(again.getDouble(tag::StopPx) != Catch::Approx(firstStop));
    // Nothing had filled: no basket is recorded for the abandoned range.
    REQUIRE(bot.runner().snapshot().baskets.empty());

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Auto profit target: a percentage of what the open positions cost, growing with every fill") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.strategy.basketProfitTargetMoney = 50.0;  // ignored while the auto target is on
    cfg.strategy.autoProfitTarget = true;
    cfg.strategy.autoTargetPercent = 100.0;
    cfg.strategy.costs.commissionPerMillionPerSide = 30.0;
    Bot bot(cfg, freshStateFile("auto_target"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // Sell Stop

    // 0.01 lot: commission 0.26 + spread 0.10 x 1 oz = 0.10 -> cost 0.36, target 100% = 0.36.
    sendQuote(server, 2, 4337.20, 4337.30);  // net 0.20 - 0.26 = -0.06
    REQUIRE(waitUntil([&] {
        const auto s = bot.runner().snapshot();
        return s.bid && *s.bid == Catch::Approx(4337.20);
    }));
    REQUIRE(bot.runner().snapshot().targetNow == Catch::Approx(0.36));
    // The same figure the engine uses on this quote.
    REQUIRE(bot.runner().snapshot().live.commission == Catch::Approx(0.26));
    sendQuote(server, 3, 4337.70, 4337.80);  // net 0.70 - 0.26 = 0.44 >= 0.36: booked
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getInt(tag::OrdType) == 1);

    bot.finish();
    bot.rethrow();
}

namespace {
void sendRejectReport(MockFixServer& server, int seq, const std::string& clOrdId, const std::string& text) {
    FixMessage r = serverHeaderMessage(msgtype::ExecutionReport, seq);
    r.set(tag::OrderID, "B" + clOrdId);
    r.set(tag::ClOrdID, clOrdId);
    r.set(tag::ExecType, std::string("8"));
    r.set(tag::OrdStatus, std::string("8"));
    r.set(tag::Text, text);
    server.sendToClient(kTradeConn, r);
}

// Opens a 0.01 lot buy (position `positionId`) and presses Close range; returns the closing order.
FixMessage openThenCloseRange(MockFixServer& server, Bot& bot, int& seq, const std::string& positionId) {
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, positionId);
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // the Sell Stop
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 1; }));
    bot.runner().closeRange();
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getString(tag::PosMaintRptID) == positionId);
    return close;
}

bool auditLogHas(const LiveSnapshot& s, const std::string& text) {
    return std::any_of(s.auditLog.begin(), s.auditLog.end(),
                       [&](const AuditEvent& e) { return e.text.find(text) != std::string::npos; });
}
}  // namespace

TEST_CASE("Audit: a rejected close whose position is already gone is booked as closed -- the basket doesn't get stuck") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.manualCloseCooldownSeconds = 0;  // no reopen after Close range
    Bot bot(cfg, freshStateFile("audit_close_gone"));
    int seq = handshake(server, {}, {});
    const FixMessage close = openThenCloseRange(server, bot, seq, "P1");

    sendRejectReport(server, seq++, close.getString(tag::ClOrdID), "POSITION_NOT_FOUND");
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {});  // the broker has nothing any more

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    const LiveSnapshot s = bot.runner().snapshot();
    REQUIRE_FALSE(s.closeFailed);
    REQUIRE_FALSE(s.closingInProgress);
    REQUIRE(s.positions.empty());
    REQUIRE(s.baskets[0].legs[0].note.find("closed at the broker") != std::string::npos);
    REQUIRE(auditLogHas(s, "already closed at the broker"));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Audit: a rejected close whose position is still open is sent again") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.manualCloseCooldownSeconds = 0;
    Bot bot(cfg, freshStateFile("audit_close_retry"));
    int seq = handshake(server, {}, {});
    const FixMessage close = openThenCloseRange(server, bot, seq, "P1");

    sendRejectReport(server, seq++, close.getString(tag::ClOrdID), "MARKET_CLOSED");
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {{"P1", true, 0.01, 4337.00}});  // still open

    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(again.getString(tag::PosMaintRptID) == "P1");
    REQUIRE(again.getString(tag::ClOrdID) != close.getString(tag::ClOrdID));
    sendFill(server, seq++, again.getString(tag::ClOrdID), 4337.50, 1.0, "P1");

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    const LiveSnapshot s = bot.runner().snapshot();
    REQUIRE_FALSE(s.closeFailed);
    REQUIRE(s.baskets[0].legs[0].exitPrice == Catch::Approx(4337.50));
    REQUIRE(auditLogHas(s, "closing it again"));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Audit: the broker's \"Unknown position\" answer to a close settles it with the broker") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.manualCloseCooldownSeconds = 0;
    Bot bot(cfg, freshStateFile("audit_unknown_position"));
    int seq = handshake(server, {}, {});
    (void)openThenCloseRange(server, bot, seq, "675409669");

    FixMessage bmr = serverHeaderMessage(msgtype::BusinessMessageReject, seq++);
    bmr.set(tag::BusinessRejectReason, int64_t{0});
    bmr.set(tag::Text, std::string("INCORRECT_POSITION_ID:Unknown position. PositionId=675409669"));
    server.sendToClient(kTradeConn, bmr);
    const FixMessage posReq = server.waitForMessage(msgtype::RequestForPositions, kTradeConn, 4s);
    seq = answerPositions(server, seq, posReq, {});

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    REQUIRE_FALSE(bot.runner().snapshot().closingInProgress);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Risk: the daily loss limit closes everything and no new range opens that day") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.dailyLossLimitMoney = 0.50;
    Bot bot(cfg, freshStateFile("daily_limit"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    (void)server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);  // the Sell Stop
    sendQuote(server, 2, 4336.00, 4336.10);  // net -$1.00, past the -$0.50 limit

    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(close.getString(tag::PosMaintRptID) == "P1");
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4336.00, 1.0, "P1");

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    const LiveSnapshot s = bot.runner().snapshot();
    REQUIRE(s.dailyLimitHit);
    REQUIRE(s.baskets[0].outcome == "daily limit");
    REQUIRE(s.dayRealized == Catch::Approx(-1.00));
    REQUIRE_FALSE(bot.runner().openRange());
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms), std::runtime_error);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lock & pause: at the loss limit the pending order is cancelled and one hedge freezes the loss") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.lockAtLossMoney = 0.50;
    cfg.connection.manualCloseCooldownSeconds = 0;
    Bot bot(cfg, freshStateFile("lock_pause"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 2, 4336.20, 4336.30);  // net -0.80: past -0.50

    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == sellStop.getString(tag::ClOrdID));
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(hedge.getInt(tag::OrdType) == 1);  // market
    REQUIRE(hedge.getInt(tag::Side) == 2);     // sells the net long 0.01
    REQUIRE(hedge.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    REQUIRE_FALSE(hedge.getOptionalString(tag::PosMaintRptID).has_value());  // a new position, not a close
    sendFill(server, seq++, hedge.getString(tag::ClOrdID), 4336.20, 1.0, "P2");

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 2; }));
    LiveSnapshot s = bot.runner().snapshot();
    REQUIRE(s.locked);
    REQUIRE(s.lockedLoss == Catch::Approx(-0.80));
    REQUIRE(s.status == EngineStatus::Idle);
    REQUIRE_FALSE(bot.runner().openRange());

    // Frozen: wherever the price goes, the net stays put (buy 0.01 @ 4337 + sell 0.01 @ 4336.20).
    sendQuote(server, 3, 4330.00, 4330.10);
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().bid && *bot.runner().snapshot().bid == Catch::Approx(4330.00); }));
    REQUIRE(bot.runner().snapshot().live.net == Catch::Approx(-0.90));  // -0.80 frozen, the hedge paid the 0.10 spread

    // Unwinding it by hand closes both; then the bot is free again.
    bot.runner().closeRange();
    const FixMessage c1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    const FixMessage c2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, c1.getString(tag::ClOrdID), 4330.00, 1.0, c1.getString(tag::PosMaintRptID));
    sendFill(server, seq++, c2.getString(tag::ClOrdID), 4330.10, 1.0, c2.getString(tag::PosMaintRptID));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    REQUIRE(bot.runner().snapshot().baskets[0].outcome == "locked");
    REQUIRE(waitUntil([&] { return !bot.runner().snapshot().locked; }, 4s));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lock & pause at a share of the balance: 25% of $2 locks at -0.50, and the first limit reached wins") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.accountBalance = 2.0;
    cfg.connection.lockAtLossPercent = 25.0;  // -0.50
    cfg.connection.lockAtLossMoney = 5.0;     // further away: the share of the balance comes first
    cfg.connection.manualCloseCooldownSeconds = 0;
    cfg.connection.recovery.guardLockMarginLevel = 0.0;  // the guards stay out of it
    cfg.connection.recovery.guardStopMarginLevel = 0.0;
    Bot bot(cfg, freshStateFile("lock_percent"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 2, 4336.60, 4336.70);  // net -0.40: not yet
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 800ms), std::runtime_error);
    sendQuote(server, 3, 4336.20, 4336.30);  // net -0.80: past -0.50
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(hedge.getInt(tag::Side) == 2);
    sendFill(server, seq++, hedge.getString(tag::ClOrdID), 4336.20, 1.0, "P2");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().locked; }));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lock at level 2: level 2 isn't placed -- a lock for the net lots waits at its line, and its fill locks") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.lockAtLevel = 2;
    cfg.connection.manualCloseCooldownSeconds = 0;
    Bot bot(cfg, freshStateFile("lock_at_level"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");

    // Instead of level 2 (Sell Stop 0.02 @ 4336): a Sell Stop for the net 0.01 at the same line.
    const FixMessage lock = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(lock.getInt(tag::OrdType) == 3);
    REQUIRE(lock.getInt(tag::Side) == 2);
    REQUIRE(lock.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(lock.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().guardLockPending; }));

    sendFill(server, seq++, lock.getString(tag::ClOrdID), 4336.00, 1.0, "P2");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().locked; }));
    REQUIRE_FALSE(bot.runner().snapshot().currentBasket->guardHeld);  // the lock level, not the margin guard

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lock, then close: a lock that holds closes the whole basket the set seconds later") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.lockAtLossMoney = 0.50;
    cfg.connection.closeLockedAfterSeconds = 1;
    cfg.connection.manualCloseCooldownSeconds = 0;
    Bot bot(cfg, freshStateFile("lock_then_close"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 2, 4336.20, 4336.30);  // net -0.80: locked
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, hedge.getString(tag::ClOrdID), 4336.20, 1.0, "P2");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 2; }));
    REQUIRE(bot.runner().snapshot().locked);

    // About a second later both sides are closed by the bot itself.
    const FixMessage c1 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    const FixMessage c2 = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(c1.getOptionalString(tag::PosMaintRptID).has_value());
    REQUIRE(c2.getOptionalString(tag::PosMaintRptID).has_value());
    sendFill(server, seq++, c1.getString(tag::ClOrdID), 4336.20, 1.0, c1.getString(tag::PosMaintRptID));
    sendFill(server, seq++, c2.getString(tag::ClOrdID), 4336.30, 1.0, c2.getString(tag::PosMaintRptID));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    REQUIRE(bot.runner().snapshot().baskets[0].outcome == "locked");
    REQUIRE(waitUntil([&] { return !bot.runner().snapshot().locked; }, 4s));

    bot.finish();
    bot.rethrow();
}

namespace {
// $20,000 at 1:1 on the $1 test range: after the first fill (buy 0.01 @ 4337)
// level 2 (Sell Stop 0.02 @ 4336) would need 0.02 x 433,600 = $8,672 of
// margin -- about 230 %, under a 300 % guard.
LiveConfig guardConfig(const MockFixServer& server) {
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.accountBalance = 20000.0;
    cfg.connection.leverage = 1.0;
    cfg.connection.recovery.guardLockMarginLevel = 300.0;
    cfg.connection.recovery.guardStopMarginLevel = 0.0;
    cfg.connection.manualCloseCooldownSeconds = 0;
    return cfg;
}
}  // namespace

TEST_CASE("Margin guard: the next level is held back and a lock stop for the net lots waits at its line") {
    MockFixServer server;
    Bot bot(guardConfig(server), freshStateFile("guard_lock_stop"));
    int seq = handshake(server, {}, {});
    sendQuote(server, 2, 4336.40, 4336.50);
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");

    const FixMessage lock = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(lock.getInt(tag::OrdType) == 3);  // a stop order, not at market
    REQUIRE(lock.getInt(tag::Side) == 2);
    REQUIRE(lock.getDouble(tag::OrderQty) == Catch::Approx(1.0));  // the net 0.01, not the level's 0.02
    REQUIRE(lock.getDouble(tag::StopPx) == Catch::Approx(4336.00));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().guardLockPending; }));
    LiveSnapshot s = bot.runner().snapshot();
    REQUIRE(s.status == EngineStatus::Idle);
    REQUIRE(auditLogHas(s, "not placed"));
    REQUIRE(s.marginLevelNow > 300.0);  // now: only the 0.01 buy

    // The price falls to the line: the lock fills and the loss is frozen.
    sendQuote(server, 3, 4335.90, 4336.00);
    sendFill(server, seq++, lock.getString(tag::ClOrdID), 4336.00, 1.0, "P2");
    REQUIRE(waitUntil([&] {
        const LiveSnapshot x = bot.runner().snapshot();
        return x.locked && !x.guardLockPending && x.positions.size() == 2;
    }));
    REQUIRE_FALSE(bot.runner().openRange());
    // No escape (off by default): nothing else is sent.
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms), std::runtime_error);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Margin guard: if the price turns back first, the basket closes at break-even and a new range opens") {
    MockFixServer server;
    LiveConfig cfg = guardConfig(server);
    cfg.connection.manualCloseCooldownSeconds = 1;  // the next range after this pause
    Bot bot(cfg, freshStateFile("guard_break_even"));
    int seq = handshake(server, {}, {});
    sendQuote(server, 2, 4336.40, 4336.50);
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage lock = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, lock.getString(tag::ClOrdID));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().guardLockPending; }));

    sendQuote(server, 3, 4337.60, 4337.70);  // net +0.60, past the $0.50 break-even amount
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == lock.getString(tag::ClOrdID));
    const FixMessage close = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(close.getString(tag::PosMaintRptID) == "P1");
    sendFill(server, seq++, close.getString(tag::ClOrdID), 4337.60, 1.0, "P1");

    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    const LiveSnapshot s = bot.runner().snapshot();
    REQUIRE(s.baskets[0].outcome == "break-even");
    REQUIRE_FALSE(s.guardLockPending);
    REQUIRE_FALSE(s.locked);
    // Auto-continue: the next range opens.
    const FixMessage next = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 4s);
    REQUIRE(next.getInt(tag::OrdType) == 3);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Margin guard: a margin level under the stop guard locks at market right away") {
    MockFixServer server;
    LiveConfig cfg = guardConfig(server);
    cfg.connection.accountBalance = 8000.0;  // buy 0.01 at 1:1 = $4,337 margin: about 184 %
    cfg.connection.recovery.guardLockMarginLevel = 0.0;
    cfg.connection.recovery.guardStopMarginLevel = 200.0;
    Bot bot(cfg, freshStateFile("guard_stop"));
    int seq = handshake(server, {}, {});
    sendQuote(server, 2, 4336.40, 4336.50);
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 3, 4336.60, 4336.70);

    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == sellStop.getString(tag::ClOrdID));
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(hedge.getInt(tag::OrdType) == 1);
    REQUIRE(hedge.getInt(tag::Side) == 2);
    REQUIRE(hedge.getDouble(tag::OrderQty) == Catch::Approx(1.0));
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().locked; }));
    REQUIRE(auditLogHas(bot.runner().snapshot(), "margin level"));

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Lock: a refused lock order is sent again (new ClOrdID) until it goes through") {
    MockFixServer server;
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.lockAtLossMoney = 0.50;
    Bot bot(cfg, freshStateFile("lock_retry"));
    int seq = handshake(server, {}, {});
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 2, 4336.20, 4336.30);
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendRejectReport(server, seq++, hedge.getString(tag::ClOrdID), "OFF_QUOTES");

    REQUIRE(waitUntil([&] { return auditLogHas(bot.runner().snapshot(), "lock") ; }));
    std::this_thread::sleep_for(600ms);
    sendQuote(server, 3, 4336.20, 4336.30);  // ticks (and the audit) drive the retry
    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::OrdType) == 1);
    REQUIRE(again.getInt(tag::Side) == 2);
    REQUIRE(again.getString(tag::ClOrdID) != hedge.getString(tag::ClOrdID));
    sendFill(server, seq++, again.getString(tag::ClOrdID), 4336.20, 1.0, "P2");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().positions.size() == 2; }));
    REQUIRE(bot.runner().snapshot().locked);

    bot.finish();
    bot.rethrow();
}

namespace {
LiveConfig escapeConfig(const MockFixServer& server) {
    LiveConfig cfg = resumeConfig(server);
    cfg.connection.lockAtLossMoney = 0.50;
    cfg.connection.accountBalance = 1000000.0;
    cfg.connection.leverage = 500.0;
    cfg.connection.recovery.escapeEnabled = true;
    cfg.connection.recovery.escapeMove = 2.0;
    cfg.connection.recovery.escapeBuffer = 0.30;
    cfg.connection.recovery.escapeTrailAt = 1.0;
    cfg.connection.recovery.escapeMaxTries = 1;
    cfg.connection.recovery.escapeAnalyzeMinutes = 0;  // no settling time in a test
    cfg.connection.recovery.escapeLookbackBars = 15;
    cfg.connection.recovery.escapeStopAtr = 1.0;
    cfg.connection.manualCloseCooldownSeconds = 0;
    return cfg;
}

// 40 finished one-minute bars before now: closes rising 0.20 a bar to
// 4336.20 (up), falling to it (down), or swinging around it (sideways).
// Each bar is 0.30 taller than its move: ATR 0.50 for the trends.
std::vector<Bar> historyBars(int trend) {
    const int64_t thisMinute = utcNowMs() / 60'000 * 60'000;
    std::vector<Bar> out;
    auto closeAt = [&](int i) {
        if (trend == 0) return 4336.20 + ((i % 2) ? 0.6 : -0.6);
        return 4336.20 + trend * 0.20 * (i - 39);
    };
    double prev = closeAt(0);
    for (int i = 0; i < 40; ++i) {
        const double c = closeAt(i);
        out.push_back(Bar{thisMinute - (40 - i) * 60'000LL, prev, std::max(prev, c) + 0.15, std::min(prev, c) - 0.15, c});
        prev = c;
    }
    return out;
}

// Buy 0.01 @ 4337, the price falls to 4336.20/30, locked with a sell 0.01 @
// 4336.20 (P2); then a quote at 4336.40/50 lets the escape decide.
void lockTheBasket(MockFixServer& server, int& seq) {
    const FixMessage buy = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, buy.getString(tag::ClOrdID), 4337.00, 1.0, "P1");
    const FixMessage sellStop = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendNewAck(server, seq++, sellStop.getString(tag::ClOrdID));
    sendQuote(server, 2, 4336.20, 4336.30);
    (void)server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    const FixMessage hedge = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, hedge.getString(tag::ClOrdID), 4336.20, 1.0, "P2");
    std::this_thread::sleep_for(200ms);
    sendQuote(server, 3, 4336.40, 4336.50);
}
}  // namespace

TEST_CASE("Escape: only with the trend -- one stop beyond the recent high in an up market; a run closes it all") {
    MockFixServer server;
    Bot bot(escapeConfig(server), freshStateFile("escape_win"));
    int seq = handshake(server, {}, {});
    bot.runner().addHistoryBars(historyBars(+1));
    lockTheBasket(server, seq);

    const FixMessage up = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(up.getInt(tag::OrdType) == 3);
    REQUIRE(up.getInt(tag::Side) == 1);  // with the trend: a Buy Stop, and no Sell Stop against it
    // Beyond the last 15 minutes' high (4336.35) and 0.30 above the ask (4336.50).
    REQUIRE(up.getDouble(tag::StopPx) == Catch::Approx(4336.80));
    REQUIRE(up.getDouble(tag::OrderQty) == Catch::Approx(1.0));  // (0.90 + costs) / $2 -> 0.01
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 800ms), std::runtime_error);

    sendFill(server, seq++, up.getString(tag::ClOrdID), 4336.80, 1.0, "P3");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().escapeTradeOpen; }));

    sendQuote(server, 4, 4339.00, 4339.10);  // frozen -0.90 + 2.20 - spread: back above break-even
    for (int i = 0; i < 3; ++i) {
        const FixMessage c = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
        REQUIRE(c.getOptionalString(tag::PosMaintRptID).has_value());
        sendFill(server, seq++, c.getString(tag::ClOrdID), 4339.00, 1.0, c.getString(tag::PosMaintRptID));
    }
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().baskets.size() == 1; }, 4s));
    REQUIRE(bot.runner().snapshot().baskets[0].outcome == "break-even");

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Escape: room to breathe -- a dip back into the range doesn't re-lock; the ATR line does, then it asks") {
    MockFixServer server;
    Bot bot(escapeConfig(server), freshStateFile("escape_whipsaw"));
    int seq = handshake(server, {}, {});
    bot.runner().addHistoryBars(historyBars(+1));
    lockTheBasket(server, seq);
    const FixMessage up = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    sendFill(server, seq++, up.getString(tag::ClOrdID), 4336.80, 1.0, "P3");
    REQUIRE(waitUntil([&] { return bot.runner().snapshot().escapeTradeOpen; }));

    // Back under the range top (4337) but above the re-lock line 4336.80 - 1 ATR = 4336.30: it holds.
    sendQuote(server, 4, 4336.60, 4336.70);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1000ms), std::runtime_error);
    REQUIRE(bot.runner().snapshot().escapeTradeOpen);

    sendQuote(server, 5, 4336.25, 4336.35);  // through the line: locked again
    const FixMessage relock = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(relock.getInt(tag::OrdType) == 1);
    REQUIRE(relock.getInt(tag::Side) == 2);
    sendFill(server, seq++, relock.getString(tag::ClOrdID), 4336.25, 1.0, "P4");
    REQUIRE(waitUntil([&] {
        const LiveSnapshot s = bot.runner().snapshot();
        return s.positions.size() == 4 && !s.escapeTradeOpen && s.escapeAwaitingDecision;
    }));

    // It waits for the operator: no new escape by itself...
    sendQuote(server, 6, 4336.40, 4336.50);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1000ms), std::runtime_error);
    // ...until Retry escape (the one try was used: Retry adds one).
    bot.runner().retryEscape();
    sendQuote(server, 7, 4336.40, 4336.50);
    const FixMessage again = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(again.getInt(tag::OrdType) == 3);
    REQUIRE(again.getInt(tag::Side) == 1);

    // Keep locked takes it back and stops escaping.
    bot.runner().keepLocked();
    const FixMessage cancel = server.waitForMessage(msgtype::OrderCancelRequest, kTradeConn, 3s);
    REQUIRE(cancel.getString(tag::OrigClOrdID) == again.getString(tag::ClOrdID));
    REQUIRE(bot.runner().snapshot().escapeHeld);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Escape: a sideways market gets no escape at all -- the basket stays locked") {
    MockFixServer server;
    Bot bot(escapeConfig(server), freshStateFile("escape_sideways"));
    int seq = handshake(server, {}, {});
    bot.runner().addHistoryBars(historyBars(0));
    lockTheBasket(server, seq);
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 1500ms), std::runtime_error);
    const LiveSnapshot s = bot.runner().snapshot();
    REQUIRE(s.locked);
    REQUIRE_FALSE(s.escapeOrdersOut);
    REQUIRE(s.escapePhase.find("no clear direction") != std::string::npos);

    bot.finish();
    bot.rethrow();
}

TEST_CASE("Escape: in a down market only a Sell Stop, below the recent low") {
    MockFixServer server;
    Bot bot(escapeConfig(server), freshStateFile("escape_down"));
    int seq = handshake(server, {}, {});
    bot.runner().addHistoryBars(historyBars(-1));
    lockTheBasket(server, seq);
    const FixMessage down = server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 3s);
    REQUIRE(down.getInt(tag::Side) == 2);
    // Below the last 15 minutes' low (4336.05) by 0.30, and 0.30 under the bid (4336.40).
    REQUIRE(down.getDouble(tag::StopPx) == Catch::Approx(4335.75));
    REQUIRE_THROWS_AS(server.waitForMessage(msgtype::NewOrderSingle, kTradeConn, 800ms), std::runtime_error);

    bot.finish();
    bot.rethrow();
}
