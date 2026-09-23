#include "muuk/ctrader_fix/live_config.hpp"

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

    requireNonEmpty(cfg.connection.brokerUid, "connection.brokerUid");
    requireNonEmpty(cfg.connection.traderLogin, "connection.traderLogin");
    requireNonEmpty(cfg.connection.password, "connection.password");

    cfg.strategy = muuk::config_json::parseStrategyConfig(j.at("strategy"));

    return cfg;
}

}  // namespace muuk::ctrader_fix
