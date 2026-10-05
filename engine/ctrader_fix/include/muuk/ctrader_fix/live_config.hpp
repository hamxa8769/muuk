#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "muuk/core/strategy_engine.hpp"
#include "muuk/ctrader_fix/market_hours.hpp"

namespace muuk::ctrader_fix {

// A single TCP endpoint (host + port) for one FIX session.
struct FixEndpoint {
    std::string host;
    std::string port;  // plain-TCP FIX port (TLS is not yet supported — see docs/ctrader-fix.md)
};

// Margin guards and what happens after a lock. The margin level is equity /
// margin in % (brokers call a margin call at 100 % or less, a stop-out at
// 50 % or less). On cTrader the equity is worked out from the typed-in
// balance, as FIX sends neither -- so the guards need accountBalance.
struct RecoveryConfig {
    // Before each next level: if the margin level after that fill would be
    // under this, the level isn't placed; a stop order for the net lots
    // (a lock) waits at that line instead. 0 = off.
    double guardLockMarginLevel = 300.0;
    // Margin level under this now: lock at market right away. 0 = off.
    double guardStopMarginLevel = 200.0;
    // How the broker charges hedged (buy + sell) lots, see core MarginRule:
    // 100 = the larger side only (cTrader), MT5 says it per symbol.
    double hedgedMarginPercent = 100.0;
    // While a lock waits or holds: once the net is back at the break-even
    // escape amount (strategy.breakEvenEscape.profit), close everything.
    bool closeAtBreakEven = true;
    // Escape (off by default), from a lock:
    //  1. wait escapeAnalyzeMinutes while the market settles and the bot
    //     reads it from 1-minute bars (trend, structure, volatility);
    //  2. only with a clear trend: ONE stop order in its direction, beyond
    //     the last escapeLookbackBars' high (or low) -- never against it, and
    //     taken back if the trend fades before it fills;
    //  3. sized to win the frozen loss back over escapeMove; the basket
    //     closes once its net is back at break-even or better;
    //  4. locked again only if the price goes escapeStopAtr x ATR against
    //     it (or back to its entry once it ran escapeTrailAt);
    //  5. after a failed try it waits for the operator (Retry escape /
    //     Close range / Keep locked), unless escapeAutoRetry.
    // At most escapeMaxTries per basket.
    bool escapeEnabled = false;
    double escapeMove = 2.0;
    double escapeBuffer = 0.30;   // the least distance beyond the breakout line / the price
    double escapeTrailAt = 1.0;
    int escapeMaxTries = 2;
    int escapeAnalyzeMinutes = 5;
    int escapeLookbackBars = 15;
    double escapeStopAtr = 1.0;
    bool escapeAutoRetry = false;
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
    // "ctrader" (default): the broker's cTrader FIX API.
    // "mt5": MetaTrader 5 through the Muuk bridge -- MuukBridge.mq5 runs in
    // MT5 and connects to the app on 127.0.0.1:mt5EaPort; the bot talks FIX
    // to the bridge on 127.0.0.1:mt5FixPort (quote/trade are set to that).
    // No FIX login is needed; traderLogin may hold the MT5 account number.
    std::string platform = "ctrader";
    int mt5EaPort = 5200;   // the bridge's pipe is \\.\pipe\MuukBridge_<this>
    int mt5FixPort = 5201;
    // MT5 is started by Muuk with the bridge on a chart (mt5AutoStart): which
    // terminal (empty = the first installed), and optionally the account to
    // log in to -- empty login = the account MT5 last used.
    bool mt5AutoStart = true;
    std::string mt5Terminal;
    std::string mt5Login;
    std::string mt5Password;
    std::string mt5Server;
    std::string mt5ChartSymbol = "XAUUSD";
    [[nodiscard]] bool isMt5() const { return platform == "mt5"; }

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

    // After the operator closes a range by hand (LiveRunner::closeRange):
    // with autoContinue on, wait this long once the account is flat, then
    // open a new range around live price. 0 = don't reopen by itself --
    // wait for Open range, like closeAllPositions/cancelAllPendingOrders.
    int manualCloseCooldownSeconds = 15;

    // A new range whose entry order hasn't triggered after this long (the
    // price walked away from it) is cancelled and opened again around the
    // live price. 0 = off: it waits for the price however long it takes.
    int entryTimeoutSeconds = 120;

    // An order the broker hasn't confirmed at all after this long is
    // checked with an Order Mass Status Request and placed again if the
    // broker doesn't have it (logged as MISSED).
    int orderAckTimeoutSeconds = 8;

    // How often (seconds) the bot asks the broker for all positions and
    // working orders and compares them with its own view. 0 = only on connect.
    int verifySeconds = 30;

    // Audit: a closing order the broker hasn't confirmed after this long is
    // checked against the broker's position list (settled or sent again).
    int closeConfirmSeconds = 10;
    // Audit: no price for this long while the market is open = a dead
    // price feed; the bot reconnects. 0 = off.
    int quoteStaleSeconds = 120;
    // Risk: once today's booked + floating net reaches minus this, the
    // running range is closed and no new range opens until tomorrow
    // (local date). 0 = off.
    double dailyLossLimitMoney = 0.0;
    // Lock & pause: once the running basket's floating net reaches minus
    // this, pending orders are cancelled and one opposite market order for
    // the net lots freezes the loss (a hedge); the bot then waits for the
    // operator. 0 = off.
    double lockAtLossMoney = 0.0;
    // The same as a share of the account balance (MT5's own, or
    // accountBalance on cTrader): 25 = lock once the basket's net reaches
    // -25% of the balance. With both set, whichever is reached first. 0 = off.
    double lockAtLossPercent = 0.0;
    // Once a lock holds (buys = sells), close the whole basket this many
    // seconds later -- the loss taken at the frozen amount instead of
    // waiting for the operator. 0 = off (it waits).
    int closeLockedAfterSeconds = 0;
    // Lock at level N: level N is never placed; a stop order for the net
    // lots (a lock) waits at its line instead. 0 = off.
    int lockAtLevel = 0;
    // Not sent over FIX -- typed in, for the risk calculator and margin warnings.
    double accountBalance = 0.0;
    double leverage = 500.0;
    // "standard" or "cent" (balance and P/L in US cents). Each account file
    // keeps its own settings, so a cent account simply has its own file.
    std::string accountKind = "standard";
    RecoveryConfig recovery;

    // true (default): open the first range as soon as the bot connects,
    // and the next one automatically after each basket closes.
    // false: only open a range when the operator asks (LiveRunner::openRange).
    bool autoContinue = true;

    // true (default): right after connecting, ask the broker for every open
    // position (Request For Positions) and show/track the ones on this
    // symbol -- including positions left over from an earlier run -- so
    // they can be closed from the app, and so a new range never opens on
    // top of them. Waits up to a few seconds for the answer.
    bool syncPositionsOnConnect = true;

    [[nodiscard]] std::string senderCompId() const {
        return environment + "." + brokerUid + "." + traderLogin;
    }
};

struct LiveConfig {
    LiveConnectionConfig connection;
    muuk::core::StrategyConfig strategy;
    MarketHoursConfig market;  // "market" in the file: sessions, pauses, news
};

// The operator-tunable subset of LiveConfig, in plain human units (lots,
// dollars, seconds), for a UI to edit without touching the fixed-point
// engine types or the credentials.
struct LiveSettings {
    double rangeWidth = 2.0;     // $ between the two boundaries
    double seedOffset = 0.0;     // $ beyond bid/ask for the two seed orders (0 = no seeding)
    double initialLot = 0.01;
    double lotIncrement = 0.01;
    double lotMultiplier = 1.0;  // > 1: each lot = previous x this (2 -> 0.01, 0.02, 0.04...); 1 = add lotIncrement
    double profitTarget = 1.0;   // basket net P/L ($) that books the basket
    double stopLoss = 0.0;       // basket net loss ($) that closes the basket; 0 = off
    bool autoTarget = false;     // target = autoTargetPercent % of the open positions' cost, instead of profitTarget
    int autoTargetPercent = 100; // 20 / 40 / 60 / 80 / 100
    bool lastLotTarget = false;     // target = the deepest filled lot x lastLotTargetMove (replaces the two above)
    double lastLotTargetMove = 1.0; // $ price move: 1.00 lot x $1 = 100 USC on a cent account
    int cooldownSeconds = 0;     // pause after a basket is fully flat
    int manualCloseCooldownSeconds = 15;  // pause after Close range before a new range opens; 0 = don't reopen
    int entryTimeoutSeconds = 120;  // entry order not triggered by then: the range moves to the live price; 0 = off
    int maxOrders = 20;
    double maxLot = 10.0;
    double maxBasketLoss = 0.0;  // $; 0 = no limit
    double dailyLossLimit = 0.0; // $; today's booked + floating net at minus this closes everything for the day; 0 = off
    double lockAtLoss = 0.0;     // $; basket net at minus this is locked (hedged) and the bot pauses; 0 = off
    double lockAtLossPercent = 0.0;  // % of the balance, the same; with both, the first reached; 0 = off
    int closeLockedAfterSeconds = 0; // a holding lock closes the whole basket this long after; 0 = waits for you
    int lockAtLevel = 0;             // level N isn't placed: the lock waits at its line instead; 0 = off
    int escapeFromLevel = 0;     // from this fill on the target drops to escapeProfit; 0 = off
    double escapeProfit = 0.50;  // $ net, after every cost
    double accountBalance = 0.0; // $, for the risk calculator
    double leverage = 500.0;     // 1:leverage
    std::string accountKind = "standard";  // or "cent"
    RecoveryConfig recovery;
    double commissionPerLot = 0.0;  // $ per 1.0 lot, round turn (open + close) -- flat-rate brokers
    // $ per $1,000,000 traded, charged on the open and again on the close
    // (cTrader's own commission type; IC Markets Raw: 30). >0 replaces commissionPerLot.
    double commissionPerMillion = 0.0;
    bool autoContinue = true;
    muuk::core::StartSide startSide = muuk::core::StartSide::Both;  // first order(s) of each range
    MarketHoursConfig market;
};

[[nodiscard]] LiveSettings settingsFromConfig(const LiveConfig& config);

// The broker login part of a config file, for a UI to show and edit. The
// password is write-only: it's never read back out (always empty here),
// and an empty password given to saveAccountSettings keeps the saved one.
struct AccountSettings {
    std::string environment = "demo";  // "demo" or "live" -- part of SenderCompID
    std::string brokerUid;
    std::string traderLogin;           // account number
    std::string password;              // write-only, see above
    std::string targetCompId = "CSERVER";
    std::string quoteHost;
    std::string quotePort;
    std::string tradeHost;
    std::string tradePort;
    std::string symbolName = "XAUUSD";
    std::string symbolId;              // empty = look it up by name on connect
    std::string platform = "ctrader";  // or "mt5" (see LiveConnectionConfig::platform)
    int mt5EaPort = 5200;
    int mt5FixPort = 5201;
    bool mt5AutoStart = true;
    std::string mt5Terminal;
    std::string mt5Login;
    std::string mt5Password;  // write-only like `password`: empty keeps the saved one
    std::string mt5Server;
    std::string mt5ChartSymbol = "XAUUSD";

    // cTrader shows the SenderCompID as "<environment>.<brokerUid>.<login>".
    [[nodiscard]] std::string senderCompId() const { return environment + "." + brokerUid + "." + traderLogin; }
    // Fills environment/brokerUid/traderLogin from a pasted SenderCompID; false if it doesn't parse.
    bool setFromSenderCompId(const std::string& senderCompId);
};

// Throws if the file can't be read or has no connection section.
[[nodiscard]] AccountSettings accountFromConfigFile(const std::filesystem::path& path);

// Writes `account` into the config file at `path`, leaving everything else
// in it (strategy settings) as it is. With `copyFrom`, the file is created
// from that one instead -- "save as a new account file", keeping the
// strategy settings. Throws std::runtime_error on invalid values (missing
// login/host, bad port, no password for a new file) or a write failure;
// written atomically, so a failure never leaves a half-written file.
void saveAccountSettings(const std::filesystem::path& path, const AccountSettings& account,
                         const std::optional<std::filesystem::path>& copyFrom = std::nullopt);

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
