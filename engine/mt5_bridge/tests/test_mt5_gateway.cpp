#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/mt5_bridge/mt5_gateway.hpp"

using namespace std::chrono_literals;
using muuk::core::Price;
using muuk::core::RangeConfig;
using muuk::core::StartSide;
using muuk::core::SymbolSpec;
using muuk::core::Volume;
using muuk::ctrader_fix::LiveConfig;
using muuk::ctrader_fix::LiveRunner;
using muuk::ctrader_fix::LiveSnapshot;
using muuk::mt5_bridge::GatewayConfig;
using muuk::mt5_bridge::Mt5Gateway;

namespace {

// Stands in for MuukBridge.mq5: connects to the gateway and speaks its JSON lines.
class FakeEa {
public:
    explicit FakeEa(uint16_t port) : socket_(io_) {
        socket_.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        reader_ = std::thread([this] { readLoop(); });
    }
    ~FakeEa() {
        asio::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        if (reader_.joinable()) reader_.join();
    }
    void send(const nlohmann::json& j) {
        const std::string line = j.dump() + "\n";
        asio::write(socket_, asio::buffer(line));
    }
    // The next command of this kind ("stop", "market", "cancel", "positions", "orders").
    nlohmann::json waitFor(const std::string& c, std::chrono::milliseconds timeout = 4s) {
        std::unique_lock lock(mutex_);
        nlohmann::json found;
        const bool ok = cv_.wait_for(lock, timeout, [&] {
            for (auto it = received_.begin(); it != received_.end(); ++it) {
                if (it->value("c", "") == c) {
                    found = *it;
                    received_.erase(it);
                    return true;
                }
            }
            return false;
        });
        if (!ok) throw std::runtime_error("FakeEa: no '" + c + "' command");
        return found;
    }
    // tickValue: account money per 0.01 per lot -- 1 when a cent lot is 1/100 of a
    // standard lot (JustMarkets), 100 when the lot is standard but money is in cents.
    void hello(bool hedging = true, double tickValue = 1.0) {
        send({{"t", "hello"},    {"login", "5550001"}, {"server", "JustMarkets-Demo"}, {"company", "JustMarkets"},
              {"currency", "USC"}, {"leverage", 1000},  {"hedging", hedging},         {"symbol", "XAUUSDc"},
              {"digits", 2},       {"contract", 100},   {"volMin", 0.01},             {"volStep", 0.01},
              {"volMax", 1000},    {"hedgedMargin", 0}, {"magic", 770077},            {"tickSize", 0.01},
              {"tickValue", tickValue}, {"marginPerLot", 433.65 * tickValue}});
        send({{"t", "account"}, {"balance", 20000.0}, {"equity", 20000.0}, {"margin", 0.0}, {"free", 20000.0},
              {"costs", nlohmann::json::array()}});
    }

private:
    void readLoop() {
        std::string buffer;
        char chunk[4096];
        while (true) {
            asio::error_code ec;
            const std::size_t n = socket_.read_some(asio::buffer(chunk), ec);
            if (ec) return;
            buffer.append(chunk, n);
            std::size_t at;
            while ((at = buffer.find('\n')) != std::string::npos) {
                const std::string line = buffer.substr(0, at);
                buffer.erase(0, at + 1);
                nlohmann::json j;
                try {
                    j = nlohmann::json::parse(line);
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "FakeEa: bad line (%s): %s\n", ex.what(), line.c_str());
                    continue;
                }
                std::scoped_lock lock(mutex_);
                received_.push_back(std::move(j));
                cv_.notify_all();
            }
        }
    }
    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<nlohmann::json> received_;
};

template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds timeout = 4s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

LiveConfig mt5Config(uint16_t fixPort) {
    LiveConfig cfg;
    cfg.connection.platform = "mt5";
    cfg.connection.quote = {"127.0.0.1", std::to_string(fixPort)};
    cfg.connection.trade = cfg.connection.quote;
    cfg.connection.brokerUid = "mt5";
    cfg.connection.traderLogin = "5550001";
    cfg.connection.password = "mt5";
    cfg.connection.targetCompId = "MT5";
    cfg.connection.heartBtIntSeconds = 1;
    cfg.connection.symbolName = "XAUUSD";  // the bridge also lists the EA's "XAUUSDc" as plain XAUUSD
    cfg.market.enabled = false;
    SymbolSpec spec;
    cfg.strategy.symbol = spec;
    cfg.strategy.range = RangeConfig{Price::fromDouble(4336.00, spec), Price::fromDouble(4337.00, spec)};
    cfg.strategy.lotProgression =
        muuk::core::LotProgressionConfig{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};
    cfg.strategy.riskLimits.maxOrders = 100;
    cfg.strategy.riskLimits.maxLot = Volume::fromDouble(10.0, spec);
    cfg.strategy.startSide = StartSide::BuyOnly;
    return cfg;
}

std::filesystem::path freshState(const std::string& name) {
    const auto file = std::filesystem::temp_directory_path() / ("muuk_test_" + name + ".state.json");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return file;
}

struct RunnerThread {
    LiveRunner& runner;
    std::thread thread;
    explicit RunnerThread(LiveRunner& r) : runner(r), thread([this] {
        try {
            runner.run();
        } catch (...) {
        }
    }) {}
    ~RunnerThread() {
        runner.stop();
        if (thread.joinable()) thread.join();
    }
};

}  // namespace

TEST_CASE("MT5 bridge: orders, fills, positions and the account go through the gateway to MT5 and back") {
    GatewayConfig gc;
    gc.eaPort = 0;
    gc.fixPort = 0;
    Mt5Gateway gateway(gc, [](const std::string&) {});
    gateway.start();
    FakeEa ea(gateway.eaPort());
    ea.hello();
    REQUIRE(waitUntil([&] { return gateway.status().ready; }));
    REQUIRE(gateway.status().currency == "USC");
    ea.send({{"t", "tick"}, {"bid", 4336.40}, {"ask", 4336.50}});

    LiveRunner runner(mt5Config(gateway.fixPort()));
    runner.setLogHandler([](const std::string&) {});
    runner.setStateFile(freshState("mt5_bridge"));
    RunnerThread thread(runner);

    // Connect-time checks: open positions and working orders at MT5 (none).
    const auto posReq = ea.waitFor("positions");
    ea.send({{"t", "positions"}, {"req", posReq.at("req")}, {"list", nlohmann::json::array()}});
    const auto ordReq = ea.waitFor("orders");
    ea.send({{"t", "orders"}, {"req", ordReq.at("req")}, {"list", nlohmann::json::array()}});
    ea.send({{"t", "tick"}, {"bid", 4336.40}, {"ask", 4336.50}});

    // The first Buy Stop reaches MT5 as a pending order.
    const auto buy = ea.waitFor("stop");
    REQUIRE(buy.at("buy") == true);
    REQUIRE(buy.at("volume").get<double>() == Catch::Approx(0.01));
    REQUIRE(buy.at("price").get<double>() == Catch::Approx(4337.00));
    const std::string buyId = buy.at("id");
    ea.send({{"t", "ack"}, {"id", buyId}, {"ticket", 9001}});
    REQUIRE(waitUntil([&] {
        const LiveSnapshot s = runner.snapshot();
        return s.pendingOrders.size() == 1 && s.pendingOrders[0].confirmed;
    }));

    // MT5 fills it: the bot has a position (MT5's position ticket) and places the next level.
    ea.send({{"t", "fill"}, {"id", buyId}, {"price", 4337.02}, {"volume", 0.01}, {"pos", "70001"}, {"buy", true}});
    REQUIRE(waitUntil([&] { return runner.snapshot().positions.size() == 1; }));
    LiveSnapshot s = runner.snapshot();
    REQUIRE(s.positions[0].positionId == "70001");
    REQUIRE(s.positions[0].entryPrice == Catch::Approx(4337.02));
    const auto sell = ea.waitFor("stop");
    REQUIRE(sell.at("buy") == false);
    REQUIRE(sell.at("volume").get<double>() == Catch::Approx(0.02));

    // The account, exactly as MT5 has it -- a cent account, with the position's swap.
    ea.send({{"t", "account"},
             {"balance", 20000.0},
             {"equity", 19999.0},
             {"margin", 43.37},
             {"free", 19955.63},
             {"costs", {{{"pos", "70001"}, {"swap", -0.25}, {"comm", 0.0}}}}});
    REQUIRE(waitUntil([&] { return runner.snapshot().brokerAccount.equity == Catch::Approx(19999.0); }));
    s = runner.snapshot();
    REQUIRE(s.platform == "mt5");
    REQUIRE(s.brokerAccount.cent());
    REQUIRE(s.brokerAccount.login == "5550001");
    REQUIRE(s.brokerAccount.leverage == Catch::Approx(1000.0));
    REQUIRE(s.equityNow == Catch::Approx(19999.0));
    REQUIRE(s.marginLevelNow == Catch::Approx(19999.0 / 43.37 * 100.0));
    REQUIRE(s.positions[0].pnl.swap == Catch::Approx(0.25));  // charged swap counts as a cost
    REQUIRE(s.moneyPerPointPerLot == Catch::Approx(100.0));   // tick value 1 per 0.01 -> 100 per 1.00
    // The margin the guards use is MT5's own: 1 lot = 433.65 at 4336.45 -> leverage about 1000.
    REQUIRE(s.marginLeverage == Catch::Approx(100.0 * 4336.45 / 433.65).epsilon(0.01));

    // Close range: a cancel for the pending order and a market close aimed at MT5's position.
    runner.closeRange(false);
    const auto cancel = ea.waitFor("cancel");
    REQUIRE(cancel.at("orig") == sell.at("id"));
    ea.send({{"t", "cancelled"}, {"id", sell.at("id")}});
    const auto close = ea.waitFor("market");
    REQUIRE(close.at("pos") == "70001");
    REQUIRE(close.at("buy") == false);
    ea.send({{"t", "fill"}, {"id", close.at("id")}, {"price", 4336.40}, {"volume", 0.01}, {"pos", "70001"}, {"buy", false}});
    REQUIRE(waitUntil([&] { return runner.snapshot().baskets.size() == 1; }));
    REQUIRE(runner.snapshot().positions.empty());
}

TEST_CASE("MT5 bridge: on a cent account with standard lots the P/L is counted in cents") {
    GatewayConfig gc;
    gc.eaPort = 0;
    gc.fixPort = 0;
    Mt5Gateway gateway(gc, [](const std::string&) {});
    gateway.start();
    FakeEa ea(gateway.eaPort());
    ea.hello(true, 100.0);  // RoboForex ProCent style: 1 lot = 100 oz, money in US cents
    REQUIRE(waitUntil([&] { return gateway.status().accountKnown; }));

    LiveRunner runner(mt5Config(gateway.fixPort()));
    runner.setLogHandler([](const std::string&) {});
    runner.setStateFile(freshState("mt5_cent_units"));
    RunnerThread thread(runner);
    const auto posReq = ea.waitFor("positions");
    ea.send({{"t", "positions"}, {"req", posReq.at("req")}, {"list", nlohmann::json::array()}});
    const auto ordReq = ea.waitFor("orders");
    ea.send({{"t", "orders"}, {"req", ordReq.at("req")}, {"list", nlohmann::json::array()}});
    ea.send({{"t", "tick"}, {"bid", 4336.40}, {"ask", 4336.50}});
    const auto buy = ea.waitFor("stop");
    ea.send({{"t", "fill"}, {"id", buy.at("id")}, {"price", 4337.00}, {"volume", 0.01}, {"pos", "71001"}, {"buy", true}});
    ea.send({{"t", "tick"}, {"bid", 4338.00}, {"ask", 4338.00}});
    REQUIRE(waitUntil([&] {
        const LiveSnapshot s = runner.snapshot();
        return s.positions.size() == 1 && s.bid && *s.bid == Catch::Approx(4338.00);
    }));
    const LiveSnapshot s = runner.snapshot();
    REQUIRE(s.moneyPerPointPerLot == Catch::Approx(10000.0));
    REQUIRE(s.positions[0].pnl.gross == Catch::Approx(100.0));  // 0.01 lot x $1 = $1 = 100 US cents
}

TEST_CASE("MT5 bridge: a netting account or a missing EA is refused at logon") {
    GatewayConfig gc;
    gc.eaPort = 0;
    gc.fixPort = 0;
    Mt5Gateway gateway(gc, [](const std::string&) {});
    gateway.start();

    SECTION("no EA connected") {
        LiveRunner runner(mt5Config(gateway.fixPort()));
        runner.setLogHandler([](const std::string&) {});
        runner.setStateFile(freshState("mt5_no_ea"));
        REQUIRE_THROWS_WITH(runner.run(), Catch::Matchers::ContainsSubstring("isn't connected"));
    }
    SECTION("netting account") {
        FakeEa ea(gateway.eaPort());
        ea.hello(false);
        REQUIRE(waitUntil([&] { return gateway.status().ready; }));
        REQUIRE_FALSE(gateway.status().hedging);
        LiveRunner runner(mt5Config(gateway.fixPort()));
        runner.setLogHandler([](const std::string&) {});
        runner.setStateFile(freshState("mt5_netting"));
        REQUIRE_THROWS_WITH(runner.run(), Catch::Matchers::ContainsSubstring("netting"));
    }
}

TEST_CASE("MT5 config: the example loads, needs no FIX login and points the bot at the local bridge") {
    const LiveConfig cfg =
        muuk::ctrader_fix::loadLiveConfig(std::string(MUUK_SOURCE_DIR) + "/engine/ctrader_fix/config.mt5.example.json");
    REQUIRE(cfg.connection.isMt5());
    REQUIRE(cfg.connection.mt5EaPort == 5200);
    REQUIRE(cfg.connection.quote.host == "127.0.0.1");
    REQUIRE(cfg.connection.quote.port == "5201");
    REQUIRE(cfg.connection.trade.port == "5201");
    REQUIRE(cfg.connection.accountKind == "cent");
    REQUIRE(cfg.strategy.lotProgression.multiplier == Catch::Approx(2.0));
}
