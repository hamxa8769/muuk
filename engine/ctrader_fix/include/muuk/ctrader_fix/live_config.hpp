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

    // true (default): open the first range as soon as the bot connects,
    // and the next one automatically after each basket closes.
    // false: only open a range when the operator asks (LiveRunner::openRange).
    bool autoContinue = true;

    [[nodiscard]] std::string senderCompId() const {
        return environment + "." + brokerUid + "." + traderLogin;
    }
};

struct LiveConfig {
    LiveConnectionConfig connection;
    muuk::core::StrategyConfig strategy;
};

// The operator-tunable subset of LiveConfig, in plain human units (lots,
// dollars, seconds), for a UI to edit without touching the fixed-point
// engine types or the credentials.
struct LiveSettings {
    double rangeWidth = 2.0;     // $ between the two boundaries
    double seedOffset = 0.0;     // $ beyond bid/ask for the two seed orders (0 = no seeding)
    double initialLot = 0.01;
    double lotIncrement = 0.01;
    double profitTarget = 1.0;   // basket net P/L ($) that books the basket
    int cooldownSeconds = 0;     // pause after a basket is fully flat
    int maxOrders = 20;
    double maxLot = 10.0;
    double maxBasketLoss = 0.0;  // $; 0 = no limit
    double commissionPerLot = 0.0;  // $ per 1.0 lot, round turn (open + close), as your broker charges it
    bool autoContinue = true;
    muuk::core::StartSide startSide = muuk::core::StartSide::Both;  // first order(s) of each range
};

[[nodiscard]] LiveSettings settingsFromConfig(const LiveConfig& config);

// Writes `settings` into `config`. The range keeps its current center;
// only its width changes (LiveRunner re-centers it on live price when a
// range is actually opened).
void applySettingsToConfig(LiveConfig& config, const LiveSettings& settings);

// Throws std::runtime_error if the file is missing, malformed, or missing
// a required connection field.
[[nodiscard]] LiveConfig loadLiveConfig(const std::filesystem::path& path);

// Rewrites only the tunable fields of an existing config file, leaving
// credentials and everything else exactly as they were. Throws
// std::runtime_error if the file can't be read or written.
void saveLiveSettings(const std::filesystem::path& path, const LiveSettings& settings);

}  // namespace muuk::ctrader_fix
