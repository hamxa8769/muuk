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

std::string jsonStr(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : v.dump();  // an account number may be written as a number
}

RecoveryConfig recoveryFromJson(const nlohmann::json& j) {
    RecoveryConfig r;
    r.guardLockMarginLevel = j.value("guardLockMarginLevel", r.guardLockMarginLevel);
    r.guardStopMarginLevel = j.value("guardStopMarginLevel", r.guardStopMarginLevel);
    r.hedgedMarginPercent = std::clamp(j.value("hedgedMarginPercent", r.hedgedMarginPercent), 0.0, 100.0);
    r.closeAtBreakEven = j.value("closeAtBreakEven", r.closeAtBreakEven);
    r.escapeEnabled = j.value("escapeEnabled", r.escapeEnabled);
    r.escapeMove = j.value("escapeMove", r.escapeMove);
    r.escapeBuffer = j.value("escapeBuffer", r.escapeBuffer);
    r.escapeTrailAt = j.value("escapeTrailAt", r.escapeTrailAt);
    r.escapeMaxTries = std::clamp(j.value("escapeMaxTries", r.escapeMaxTries), 0, 10);
    r.escapeAnalyzeMinutes = std::clamp(j.value("escapeAnalyzeMinutes", r.escapeAnalyzeMinutes), 0, 240);
    r.escapeLookbackBars = std::clamp(j.value("escapeLookbackBars", r.escapeLookbackBars), 4, 120);
    r.escapeStopAtr = std::clamp(j.value("escapeStopAtr", r.escapeStopAtr), 0.2, 10.0);
    r.escapeAutoRetry = j.value("escapeAutoRetry", r.escapeAutoRetry);
    return r;
}

nlohmann::json recoveryToJson(const RecoveryConfig& r) {
    return {{"guardLockMarginLevel", r.guardLockMarginLevel},
            {"guardStopMarginLevel", r.guardStopMarginLevel},
            {"hedgedMarginPercent", r.hedgedMarginPercent},
            {"closeAtBreakEven", r.closeAtBreakEven},
            {"escapeEnabled", r.escapeEnabled},
            {"escapeMove", r.escapeMove},
            {"escapeBuffer", r.escapeBuffer},
            {"escapeTrailAt", r.escapeTrailAt},
            {"escapeMaxTries", r.escapeMaxTries},
            {"escapeAnalyzeMinutes", r.escapeAnalyzeMinutes},
            {"escapeLookbackBars", r.escapeLookbackBars},
            {"escapeStopAtr", r.escapeStopAtr},
            {"escapeAutoRetry", r.escapeAutoRetry}};
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
    cfg.connection.platform = conn.value("platform", std::string("ctrader"));
    if (conn.contains("mt5")) {
        const auto& m = conn.at("mt5");
        cfg.connection.mt5EaPort = m.value("eaPort", 5200);
        cfg.connection.mt5FixPort = m.value("fixPort", 5201);
        cfg.connection.mt5AutoStart = m.value("autoStart", true);
        cfg.connection.mt5Terminal = m.value("terminal", std::string());
        cfg.connection.mt5Login = jsonStr(m, "login");
        cfg.connection.mt5Password = m.value("password", std::string());
        cfg.connection.mt5Server = m.value("server", std::string());
        cfg.connection.mt5ChartSymbol = m.value("chartSymbol", std::string("XAUUSD"));
    }
    const bool mt5 = cfg.connection.isMt5();
    if (mt5) {
        // The bridge on this PC is the "broker".
        cfg.connection.quote = FixEndpoint{"127.0.0.1", std::to_string(cfg.connection.mt5FixPort)};
        cfg.connection.trade = cfg.connection.quote;
    } else {
        cfg.connection.quote = parseEndpoint(conn.at("quote"), "connection.quote");
        cfg.connection.trade = parseEndpoint(conn.at("trade"), "connection.trade");
    }
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
    cfg.connection.manualCloseCooldownSeconds = conn.value("manualCloseCooldownSeconds", 15);
    cfg.connection.entryTimeoutSeconds = std::max(0, conn.value("entryTimeoutSeconds", 120));
    cfg.connection.orderAckTimeoutSeconds = conn.value("orderAckTimeoutSeconds", 8);
    cfg.connection.verifySeconds = conn.value("verifySeconds", 30);
    cfg.connection.closeConfirmSeconds = conn.value("closeConfirmSeconds", 10);
    cfg.connection.quoteStaleSeconds = conn.value("quoteStaleSeconds", 120);
    cfg.connection.dailyLossLimitMoney = conn.value("dailyLossLimitMoney", 0.0);
    cfg.connection.lockAtLossMoney = conn.value("lockAtLossMoney", 0.0);
    cfg.connection.lockAtLossPercent = std::clamp(conn.value("lockAtLossPercent", 0.0), 0.0, 100.0);
    cfg.connection.closeLockedAfterSeconds = std::max(0, conn.value("closeLockedAfterSeconds", 0));
    cfg.connection.accountBalance = conn.value("accountBalance", 0.0);
    cfg.connection.leverage = conn.value("leverage", 500.0);
    cfg.connection.accountKind = conn.value("accountKind", std::string("standard"));
    if (conn.contains("recovery")) cfg.connection.recovery = recoveryFromJson(conn.at("recovery"));
    cfg.connection.autoContinue = conn.value("autoContinue", true);
    cfg.connection.syncPositionsOnConnect = conn.value("syncPositionsOnConnect", true);

    if (mt5) {
        if (cfg.connection.brokerUid.empty()) cfg.connection.brokerUid = "mt5";
        if (cfg.connection.traderLogin.empty()) cfg.connection.traderLogin = "mt5";
        if (cfg.connection.password.empty()) cfg.connection.password = "mt5";
        if (cfg.connection.targetCompId == "CSERVER") cfg.connection.targetCompId = "MT5";
    } else {
        requireNonEmpty(cfg.connection.brokerUid, "connection.brokerUid");
        requireNonEmpty(cfg.connection.traderLogin, "connection.traderLogin");
        requireNonEmpty(cfg.connection.password, "connection.password");
    }

    cfg.strategy = muuk::config_json::parseStrategyConfig(j.at("strategy"));
    if (j.contains("market")) cfg.market = marketHoursFromJson(j.at("market"));
    // Auto-target levels 20..100 are 200..360 % of the cost.
    cfg.strategy.autoTargetPercent = std::clamp(cfg.strategy.autoTargetPercent, 200.0, 360.0);

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
    s.lotMultiplier = config.strategy.lotProgression.multiplier;
    s.profitTarget = config.strategy.basketProfitTargetMoney;
    s.stopLoss = config.strategy.basketStopLossMoney;
    s.autoTarget = config.strategy.autoProfitTarget;
    s.autoTargetPercent = static_cast<int>(std::lround(config.strategy.autoTargetPercent));
    s.lastLotTarget = config.strategy.lastLotTarget;
    s.lastLotTargetMove = config.strategy.lastLotTargetMove;
    s.market = config.market;
    s.cooldownSeconds = config.connection.basketCooldownSeconds;
    s.manualCloseCooldownSeconds = config.connection.manualCloseCooldownSeconds;
    s.entryTimeoutSeconds = config.connection.entryTimeoutSeconds;
    s.maxOrders = config.strategy.riskLimits.maxOrders;
    s.maxLot = config.strategy.riskLimits.maxLot.toDouble(spec);
    s.maxBasketLoss = config.strategy.riskLimits.maxBasketLossMoney;
    s.dailyLossLimit = config.connection.dailyLossLimitMoney;
    s.lockAtLoss = config.connection.lockAtLossMoney;
    s.lockAtLossPercent = config.connection.lockAtLossPercent;
    s.closeLockedAfterSeconds = config.connection.closeLockedAfterSeconds;
    s.escapeFromLevel = config.strategy.escapeFromLevel;
    s.escapeProfit = config.strategy.escapeProfitMoney;
    s.accountBalance = config.connection.accountBalance;
    s.leverage = config.connection.leverage;
    s.accountKind = config.connection.accountKind;
    s.recovery = config.connection.recovery;
    s.commissionPerLot = config.strategy.costs.commissionPerLotRoundTurn;
    s.commissionPerMillion = config.strategy.costs.commissionPerMillionPerSide;
    s.autoContinue = config.connection.autoContinue;
    s.startSide = config.strategy.startSide;
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
                                              Volume::fromDouble(s.lotIncrement, spec), s.lotMultiplier};
    st.basketProfitTargetMoney = s.profitTarget;
    st.basketStopLossMoney = s.stopLoss;
    st.autoProfitTarget = s.autoTarget;
    st.autoTargetPercent = s.autoTargetPercent;
    st.lastLotTarget = s.lastLotTarget;
    st.lastLotTargetMove = std::clamp(s.lastLotTargetMove, 0.01, 100.0);
    config.market = s.market;
    st.riskLimits.maxOrders = s.maxOrders;
    st.riskLimits.maxLot = Volume::fromDouble(s.maxLot, spec);
    st.riskLimits.maxBasketLossMoney = s.maxBasketLoss;
    config.connection.dailyLossLimitMoney = s.dailyLossLimit;
    config.connection.lockAtLossMoney = s.lockAtLoss;
    config.connection.lockAtLossPercent = std::clamp(s.lockAtLossPercent, 0.0, 100.0);
    config.connection.closeLockedAfterSeconds = std::max(0, s.closeLockedAfterSeconds);
    config.connection.accountBalance = s.accountBalance;
    config.connection.leverage = s.leverage;
    config.connection.accountKind = s.accountKind;
    config.connection.recovery = s.recovery;
    st.escapeFromLevel = s.escapeFromLevel;
    st.escapeProfitMoney = s.escapeProfit;
    st.costs.commissionPerLotRoundTurn = s.commissionPerLot;
    st.costs.commissionPerMillionPerSide = s.commissionPerMillion;
    st.startSide = s.startSide;

    config.connection.autoRangeWidthPoints = s.rangeWidth;
    config.connection.basketCooldownSeconds = s.cooldownSeconds;
    config.connection.manualCloseCooldownSeconds = s.manualCloseCooldownSeconds;
    config.connection.entryTimeoutSeconds = std::max(0, s.entryTimeoutSeconds);
    config.connection.autoContinue = s.autoContinue;
}

bool AccountSettings::setFromSenderCompId(const std::string& id) {
    const auto first = id.find('.');
    const auto last = id.rfind('.');
    if (first == std::string::npos || last == first || last + 1 >= id.size()) return false;
    environment = id.substr(0, first);
    brokerUid = id.substr(first + 1, last - first - 1);
    traderLogin = id.substr(last + 1);
    return !environment.empty() && !brokerUid.empty() && !traderLogin.empty();
}

namespace {
nlohmann::json readJsonFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path.string());
    nlohmann::json j;
    in >> j;
    return j;
}

void writeJsonFileAtomically(const std::filesystem::path& path, const nlohmann::json& j) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << j.dump(2);
        out.flush();
        if (!out) throw std::runtime_error("write failed: " + tmp.string());
    }
    std::filesystem::rename(tmp, path);
}

std::string jsonString(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : v.dump();  // ports may have been written as numbers
}
}  // namespace

AccountSettings accountFromConfigFile(const std::filesystem::path& path) {
    const nlohmann::json j = readJsonFile(path);
    if (!j.contains("connection")) throw std::runtime_error("no \"connection\" section in " + path.string());
    const auto& c = j.at("connection");
    AccountSettings a;
    a.environment = c.value("environment", std::string("demo"));
    a.brokerUid = jsonString(c, "brokerUid");
    a.traderLogin = jsonString(c, "traderLogin");
    a.targetCompId = c.value("targetCompId", std::string("CSERVER"));
    if (c.contains("quote")) {
        a.quoteHost = jsonString(c.at("quote"), "host");
        a.quotePort = jsonString(c.at("quote"), "port");
    }
    if (c.contains("trade")) {
        a.tradeHost = jsonString(c.at("trade"), "host");
        a.tradePort = jsonString(c.at("trade"), "port");
    }
    a.symbolName = c.value("symbolName", std::string("XAUUSD"));
    a.symbolId = jsonString(c, "symbolId");
    a.platform = c.value("platform", std::string("ctrader"));
    if (c.contains("mt5")) {
        const auto& m = c.at("mt5");
        a.mt5EaPort = m.value("eaPort", 5200);
        a.mt5FixPort = m.value("fixPort", 5201);
        a.mt5AutoStart = m.value("autoStart", true);
        a.mt5Terminal = m.value("terminal", std::string());
        a.mt5Login = jsonString(m, "login");
        a.mt5Server = m.value("server", std::string());
        a.mt5ChartSymbol = m.value("chartSymbol", std::string("XAUUSD"));
    }
    return a;  // password deliberately left empty
}

void saveAccountSettings(const std::filesystem::path& path, const AccountSettings& a,
                         const std::optional<std::filesystem::path>& copyFrom) {
    auto require = [](const std::string& v, const char* what) {
        if (v.empty()) throw std::runtime_error(std::string(what) + " is empty");
    };
    auto requirePort = [](const std::string& v, const char* what) {
        if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos || v.size() > 5) {
            throw std::runtime_error(std::string(what) + " must be a port number");
        }
    };
    if (a.platform == "mt5") {
        // MetaTrader 5 through the bridge: no FIX login -- just the two local ports.
        if (a.mt5EaPort <= 0 || a.mt5EaPort > 65535 || a.mt5FixPort <= 0 || a.mt5FixPort > 65535 ||
            a.mt5EaPort == a.mt5FixPort) {
            throw std::runtime_error("MT5 bridge ports must be two different numbers from 1 to 65535");
        }
        nlohmann::json j = readJsonFile(copyFrom ? *copyFrom : path);
        auto& c = j["connection"];
        c["platform"] = "mt5";
        const std::string oldPassword =
            !copyFrom && c.contains("mt5") && c.at("mt5").contains("password") && c.at("mt5").at("password").is_string()
                ? c.at("mt5").at("password").get<std::string>()
                : std::string();
        c["mt5"] = {{"eaPort", a.mt5EaPort},       {"fixPort", a.mt5FixPort}, {"autoStart", a.mt5AutoStart},
                    {"terminal", a.mt5Terminal},   {"login", a.mt5Login},     {"server", a.mt5Server},
                    {"chartSymbol", a.mt5ChartSymbol.empty() ? std::string("XAUUSD") : a.mt5ChartSymbol}};
        const std::string password = !a.mt5Password.empty() ? a.mt5Password : oldPassword;
        if (!password.empty()) c["mt5"]["password"] = password;
        c["environment"] = a.environment;
        c["traderLogin"] = a.traderLogin;  // the MT5 account number (only a label here)
        c["symbolName"] = a.symbolName;
        if (copyFrom) {
            // A new MT5 file never inherits another account's FIX login.
            c.erase("password");
            c.erase("brokerUid");
            c["symbolId"] = "";
        }
        std::error_code ec;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        writeJsonFileAtomically(path, j);
        return;
    }
    if (a.environment != "demo" && a.environment != "live") throw std::runtime_error("environment must be demo or live");
    require(a.brokerUid, "Broker UID");
    require(a.traderLogin, "Account number");
    require(a.targetCompId, "TargetCompID");
    require(a.quoteHost, "Price host");
    require(a.tradeHost, "Trade host");
    requirePort(a.quotePort, "Price port");
    requirePort(a.tradePort, "Trade port");
    require(a.symbolName, "Symbol");

    nlohmann::json j = readJsonFile(copyFrom ? *copyFrom : path);
    auto& c = j["connection"];
    const std::string oldPassword = c.contains("password") && c.at("password").is_string()
                                        ? c.at("password").get<std::string>()
                                        : std::string();
    const std::string oldSymbol = c.value("symbolName", std::string());
    c["environment"] = a.environment;
    c["brokerUid"] = a.brokerUid;
    c["traderLogin"] = a.traderLogin;
    c["targetCompId"] = a.targetCompId;
    c["quote"] = {{"host", a.quoteHost}, {"port", a.quotePort}};
    c["trade"] = {{"host", a.tradeHost}, {"port", a.tradePort}};
    c["symbolName"] = a.symbolName;
    // A symbol id belongs to one broker's symbol list: keep it only if it was given, or the symbol didn't change.
    c["symbolId"] = !a.symbolId.empty() ? a.symbolId : (a.symbolName == oldSymbol && !copyFrom ? jsonString(c, "symbolId") : "");
    c["platform"] = "ctrader";
    if (!a.password.empty()) {
        c["password"] = a.password;
    } else if (oldPassword.empty() || copyFrom) {
        // A new account file never inherits another account's password.
        throw std::runtime_error("Password is empty");
    }
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    writeJsonFileAtomically(path, j);
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
    conn["manualCloseCooldownSeconds"] = s.manualCloseCooldownSeconds;
    conn["entryTimeoutSeconds"] = s.entryTimeoutSeconds;
    conn["autoContinue"] = s.autoContinue;
    conn["dailyLossLimitMoney"] = s.dailyLossLimit;
    conn["lockAtLossMoney"] = s.lockAtLoss;
    conn["lockAtLossPercent"] = s.lockAtLossPercent;
    conn["closeLockedAfterSeconds"] = s.closeLockedAfterSeconds;
    conn["accountBalance"] = s.accountBalance;
    conn["leverage"] = s.leverage;
    conn["accountKind"] = s.accountKind;
    conn["recovery"] = recoveryToJson(s.recovery);
    j["strategy"]["breakEvenEscape"] = {{"fromLevel", s.escapeFromLevel}, {"profit", s.escapeProfit}};

    auto& st = j["strategy"];
    st["seedOffsetPoints"] = s.seedOffset;
    st["startSide"] = muuk::config_json::startSideToString(s.startSide);
    st["basketProfitTargetMoney"] = s.profitTarget;
    st["basketStopLossMoney"] = s.stopLoss;
    st["autoProfitTarget"] = {{"enabled", s.autoTarget}, {"percent", s.autoTargetPercent}};
    st["lastLotTarget"] = {{"enabled", s.lastLotTarget}, {"move", s.lastLotTargetMove}};
    j["market"] = marketHoursToJson(s.market);
    st["lotProgression"]["initialLot"] = s.initialLot;
    st["lotProgression"]["lotIncrement"] = s.lotIncrement;
    st["lotProgression"]["lotMultiplier"] = s.lotMultiplier;
    st["riskLimits"]["maxOrders"] = s.maxOrders;
    st["riskLimits"]["maxLot"] = s.maxLot;
    st["riskLimits"]["maxBasketLossMoney"] = s.maxBasketLoss;
    st["costs"]["commissionPerLotRoundTurn"] = s.commissionPerLot;
    st["costs"]["commissionPerMillionPerSide"] = s.commissionPerMillion;

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write live config file: " + path.string());
    }
    out << j.dump(2) << "\n";
}

}  // namespace muuk::ctrader_fix
