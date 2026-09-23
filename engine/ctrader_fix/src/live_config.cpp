#include "muuk/ctrader_fix/live_config.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "muuk/config_json/strategy_config_json.hpp"

namespace muuk::ctrader_fix {

namespace {

void requireNonEmpty(const std::string& value, const char* fieldName) {
    if (value.empty()) {
        throw std::runtime_error(std::string("live config: missing required field 'connection.") + fieldName +
                                  "'");
    }
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
    cfg.connection.host = conn.value("host", "");
    cfg.connection.port = conn.value("port", "");
    cfg.connection.environment = conn.value("environment", std::string("demo"));
    cfg.connection.brokerUid = conn.value("brokerUid", "");
    cfg.connection.traderLogin = conn.value("traderLogin", "");
    cfg.connection.password = conn.value("password", "");
    cfg.connection.heartBtIntSeconds = conn.value("heartBtIntSeconds", 30);
    cfg.connection.symbolName = conn.value("symbolName", std::string("XAUUSD"));

    requireNonEmpty(cfg.connection.host, "host");
    requireNonEmpty(cfg.connection.port, "port");
    requireNonEmpty(cfg.connection.brokerUid, "brokerUid");
    requireNonEmpty(cfg.connection.traderLogin, "traderLogin");
    requireNonEmpty(cfg.connection.password, "password");

    cfg.strategy = muuk::config_json::parseStrategyConfig(j.at("strategy"));

    return cfg;
}

}  // namespace muuk::ctrader_fix
