#pragma once

#include <filesystem>
#include <string>

#include "muuk/core/strategy_engine.hpp"

namespace muuk::ctrader_fix {

// Connection/credential details for one cTrader FIX account. Loaded from a
// local, gitignored JSON file — never hardcoded, never committed. See
// docs/ctrader-fix.md for where to get these values (cTrader platform:
// Settings -> FIX API) and README.md for the file's expected location.
struct LiveConnectionConfig {
    std::string host;
    std::string port;                  // plain-TCP FIX port from your cTrader FIX API settings (TLS is not yet supported — see docs/ctrader-fix.md)
    std::string environment = "demo";  // "demo" or "live" — used to build SenderCompID
    std::string brokerUid;
    std::string traderLogin;           // numeric account id
    std::string password;
    int heartBtIntSeconds = 30;
    std::string symbolName = "XAUUSD";

    [[nodiscard]] std::string senderCompId() const {
        return environment + "." + brokerUid + "." + traderLogin;
    }
};

struct LiveConfig {
    LiveConnectionConfig connection;
    muuk::core::StrategyConfig strategy;
};

// Throws std::runtime_error if the file is missing, malformed, or missing
// a required connection field.
[[nodiscard]] LiveConfig loadLiveConfig(const std::filesystem::path& path);

}  // namespace muuk::ctrader_fix
