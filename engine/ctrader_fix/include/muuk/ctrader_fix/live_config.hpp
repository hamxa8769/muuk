#pragma once

#include <filesystem>
#include <string>

#include "muuk/core/strategy_engine.hpp"

namespace muuk::ctrader_fix {

// A single TCP endpoint (host + port) for one FIX session.
struct FixEndpoint {
    std::string host;
    std::string port;  // plain-TCP FIX port (TLS is not yet supported — see docs/ctrader-fix.md)
};

// Connection/credential details for one cTrader FIX account. Loaded from a
// local, gitignored JSON file — never hardcoded, never committed. See
// docs/ctrader-fix.md for where to get these values (cTrader platform:
// Settings -> FIX API) and README.md for the file's expected location.
//
// cTrader's settings screen lists the Price (Quote) and Trade connections
// separately, each with its own Host/Port — they are not assumed to share
// one address. The account identity (SenderCompID/Username/Password) is
// the same for both; only the endpoint and TargetSubID/SenderSubID differ
// (handled by LiveRunner, not part of this config).
struct LiveConnectionConfig {
    FixEndpoint quote;  // "Price connection" in cTrader's settings screen
    FixEndpoint trade;
    std::string environment = "demo";  // "demo" or "live" — used to build SenderCompID
    std::string brokerUid;
    std::string traderLogin;  // numeric account id
    std::string password;
    // Shown as "TargetCompID" on your cTrader FIX API settings screen.
    // The generic spec examples use "CSERVER", but individual
    // brokers/accounts can (and do) use a different exact string here
    // (e.g. "cServer") — this must match yours exactly or Logon fails.
    std::string targetCompId = "CSERVER";
    int heartBtIntSeconds = 30;
    std::string symbolName = "XAUUSD";
    // Optional: if your account's FIX symbol id for `symbolName` is
    // already known (shown on the same settings screen), set it here to
    // skip the SecurityListRequest lookup at startup. Leave empty to
    // resolve it automatically instead.
    std::string symbolId;

    // 0 (default): use strategy.range exactly as configured.
    // >0: ignore strategy.range's lower/upper and instead center a range
    // of this total width around the first live quote received after
    // connecting — e.g. 0.20 with a mid price of 4292.45 gives a range of
    // [4292.35, 4292.55]. This is what makes "just start it, no need to
    // look up the current price by hand" possible; see
    // LiveRunner::waitForFirstQuoteAndCenterRange in live_runner.cpp.
    double autoRangeWidthPoints = 0.0;

    // How long LiveRunner waits, after a basket has fully closed (every
    // filled leg confirmed flattened at the broker), before starting the
    // next cycle's range. 0 (default) starts immediately, matching the
    // behavior before this existed. A pause here is purely operational —
    // e.g. giving the operator a moment to glance at the account, or
    // avoiding immediately re-seeding into the same brief spike that just
    // closed the previous basket — engine/core itself has no notion of
    // wall-clock time.
    int basketCooldownSeconds = 0;

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
