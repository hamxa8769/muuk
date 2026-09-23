#include "muuk/ctrader_fix/live_config.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "muuk/config_json/strategy_config_json.hpp"

namespace muuk::ctrader_fix {

namespace {

void requireNonEmpty(const std::string& value, const std::string& fieldPath) {
    if (value.empty()) {
        throw std::runtime_error("live config: missing required field '" + fieldPath + "'");
    }
}

FixEndpoint parseEndpoint(const nlohmann::json& j, const std::string& fieldPath) {
    FixEndpoint endpoint;
    endpoint.host = j.value("host", "");
    endpoint.port = j.value("port", "");
    requireNonEmpty(endpoint.host, fieldPath + ".host");
    requireNonEmpty(endpoint.port, fieldPath + ".port");
    return endpoint;
}

}  // namespace

LiveConfig loadLiveConfig(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open live config file: " + path.string());
    }

    nlohmann::json j;
    file >> j;

    LiveConfig cfg;
    const auto& conn = j.at("connection");
    cfg.connection.quote = parseEndpoint(conn.at("quote"), "connection.quote");
    cfg.connection.trade = parseEndpoint(conn.at("trade"), "connection.trade");
    cfg.connection.environment = conn.value("environment", std::string("demo"));
    cfg.connection.brokerUid = conn.value("brokerUid", "");
    cfg.connection.traderLogin = conn.value("traderLogin", "");
    cfg.connection.password = conn.value("password", "");
    cfg.connection.targetCompId = conn.value("targetCompId", std::string("CSERVER"));
    cfg.connection.heartBtIntSeconds = conn.value("heartBtIntSeconds", 30);
    cfg.connection.symbolName = conn.value("symbolName", std::string("XAUUSD"));
    cfg.connection.symbolId = conn.value("symbolId", "");
    cfg.connection.autoRangeWidthPoints = conn.value("autoRangeWidthPoints", 0.0);
    cfg.connection.basketCooldownSeconds = conn.value("basketCooldownSeconds", 0);
    cfg.connection.autoContinue = conn.value("autoContinue", true);

    requireNonEmpty(cfg.connection.brokerUid, "connection.brokerUid");
    requireNonEmpty(cfg.connection.traderLogin, "connection.traderLogin");
    requireNonEmpty(cfg.connection.password, "connection.password");

    cfg.strategy = muuk::config_json::parseStrategyConfig(j.at("strategy"));

    return cfg;
}

LiveSettings settingsFromConfig(const LiveConfig& config) {
    using namespace muuk::core;
    const SymbolSpec& spec = config.strategy.symbol;
    LiveSettings s;
    s.rangeWidth = config.connection.autoRangeWidthPoints > 0.0
                       ? config.connection.autoRangeWidthPoints
                       : (config.strategy.range.upperBound - config.strategy.range.lowerBound).toDouble(spec);
    s.seedOffset = config.strategy.seedOffsetPoints;
    s.initialLot = config.strategy.lotProgression.initialLot.toDouble(spec);
    s.lotIncrement = config.strategy.lotProgression.lotIncrement.toDouble(spec);
    s.profitTarget = config.strategy.basketProfitTargetMoney;
    s.cooldownSeconds = config.connection.basketCooldownSeconds;
    s.maxOrders = config.strategy.riskLimits.maxOrders;
    s.maxLot = config.strategy.riskLimits.maxLot.toDouble(spec);
    s.maxBasketLoss = config.strategy.riskLimits.maxBasketLossMoney;
    s.autoContinue = config.connection.autoContinue;
    return s;
}

void applySettingsToConfig(LiveConfig& config, const LiveSettings& s) {
    using namespace muuk::core;
    StrategyConfig& st = config.strategy;
    const SymbolSpec& spec = st.symbol;

    const int64_t center = (st.range.lowerBound.ticks + st.range.upperBound.ticks) / 2;
    const int64_t halfWidth = std::max<int64_t>(1, Price::fromDouble(s.rangeWidth, spec).ticks / 2);
    st.range = RangeConfig{Price{center - halfWidth}, Price{center + halfWidth}};

    st.seedOffsetPoints = s.seedOffset;
    st.lotProgression = LotProgressionConfig{Volume::fromDouble(s.initialLot, spec),
                                              Volume::fromDouble(s.lotIncrement, spec)};
    st.basketProfitTargetMoney = s.profitTarget;
    st.riskLimits.maxOrders = s.maxOrders;
    st.riskLimits.maxLot = Volume::fromDouble(s.maxLot, spec);
    st.riskLimits.maxBasketLossMoney = s.maxBasketLoss;

    config.connection.autoRangeWidthPoints = s.rangeWidth;
    config.connection.basketCooldownSeconds = s.cooldownSeconds;
    config.connection.autoContinue = s.autoContinue;
}

void saveLiveSettings(const std::filesystem::path& path, const LiveSettings& s) {
    nlohmann::json j;
    {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("cannot open live config file: " + path.string());
        }
        in >> j;
    }

    auto& conn = j["connection"];
    conn["autoRangeWidthPoints"] = s.rangeWidth;
    conn["basketCooldownSeconds"] = s.cooldownSeconds;
    conn["autoContinue"] = s.autoContinue;

    auto& st = j["strategy"];
    st["seedOffsetPoints"] = s.seedOffset;
    st["basketProfitTargetMoney"] = s.profitTarget;
    st["lotProgression"]["initialLot"] = s.initialLot;
    st["lotProgression"]["lotIncrement"] = s.lotIncrement;
    st["riskLimits"]["maxOrders"] = s.maxOrders;
    st["riskLimits"]["maxLot"] = s.maxLot;
    st["riskLimits"]["maxBasketLossMoney"] = s.maxBasketLoss;

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write live config file: " + path.string());
    }
    out << j.dump(2) << "\n";
}

}  // namespace muuk::ctrader_fix
