// State persistence: a basket in progress survives the bot being stopped
// and started again (LiveRunner::setStateFile).

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
