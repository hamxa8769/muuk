#include "live_controller.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <fstream>
#include <stdexcept>

#include "muuk/ctrader_fix/basket_history.hpp"
#include "muuk/ctrader_fix/live_state.hpp"
#include "muuk/ctrader_fix/session_report.hpp"

namespace muuk::desktop {

using muuk::ctrader_fix::LiveConfig;
using muuk::ctrader_fix::LiveRunner;
using muuk::ctrader_fix::LiveSettings;
using muuk::ctrader_fix::LiveSnapshot;

namespace {
std::string timestampPrefix() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S  ", &local);
    return buf;
}
}  // namespace

LiveController::~LiveController() {
    stop();
    if (news_) news_->stop();
    if (mt5Thread_.joinable()) mt5Thread_.join();
    if (bridge_) bridge_->stop();
}

void LiveController::launchMt5(const LiveConfig& config) {
    if (mt5Starting_.exchange(true)) return;
    if (mt5Thread_.joinable()) mt5Thread_.join();
    muuk::mt5_bridge::Mt5StartOptions o;
    o.terminal = config.connection.mt5Terminal;
    o.login = config.connection.mt5Login;
    o.password = config.connection.mt5Password;
    o.server = config.connection.mt5Server;
    o.chartSymbol = config.connection.mt5ChartSymbol;
    o.pipeName = "MuukBridge_" + std::to_string(config.connection.mt5EaPort);
    {
        std::scoped_lock lock(mt5Mutex_);
        mt5Message_ = "Starting MetaTrader 5...";
    }
    mt5Thread_ = std::thread([this, o] {
        const auto r = muuk::mt5_bridge::startMt5WithBridge(o, [this](const std::string& line) { appendLog(line); });
        {
            std::scoped_lock lock(mt5Mutex_);
            mt5Message_ = r.message;
        }
        if (!r.started && !r.alreadyRunning) appendLog("[mt5] " + r.message);
        mt5Starting_ = false;
    });
}

void LiveController::startMt5(const std::string& configPath) {
    try {
        const LiveConfig config = muuk::ctrader_fix::loadLiveConfig(configPath);
        if (!config.connection.isMt5()) return;
        if (!ensureBridge(config)) return;
        launchMt5(config);
    } catch (const std::exception& ex) {
        setError(ex.what());
    }
}

std::string LiveController::mt5StartMessage() const {
    std::scoped_lock lock(mt5Mutex_);
    return mt5Message_;
}

bool LiveController::ensureBridge(const LiveConfig& config) {
    if (!config.connection.isMt5()) return true;
    if (bridge_ && bridgeEaPort_ == config.connection.mt5EaPort && bridgeFixPort_ == config.connection.mt5FixPort) {
        return true;
    }
    if (bridge_) bridge_->stop();
    muuk::mt5_bridge::GatewayConfig gc;
    gc.pipeName = "MuukBridge_" + std::to_string(config.connection.mt5EaPort);
    gc.fixPort = static_cast<uint16_t>(config.connection.mt5FixPort);
    bridge_ = std::make_unique<muuk::mt5_bridge::Mt5Gateway>(gc, [this](const std::string& line) { appendLog(line); });
    try {
        bridge_->start();
    } catch (const std::exception& ex) {
        bridge_.reset();
        setError(ex.what());
        return false;
    }
    bridgeEaPort_ = config.connection.mt5EaPort;
    bridgeFixPort_ = config.connection.mt5FixPort;
    return true;
}

std::optional<muuk::mt5_bridge::EaStatus> LiveController::bridgeStatus() const {
    if (!bridge_) return std::nullopt;
    return bridge_->status();
}

void LiveController::startNews(const std::filesystem::path& cacheFile) {
    if (news_) return;
    news_ = std::make_shared<muuk::ctrader_fix::NewsFeed>(cacheFile);
    news_->start();
}

void LiveController::start(const std::string& configPath) {
    if (running_.load() || demo_) return;  // the preview never connects
    if (!instanceLock_.acquire(muuk::ctrader_fix::instanceLockFile(configPath))) {
        setError("Muuk is already running with this config (another window, muuk-live, or the auto-start task). "
                 "Close that one first -- two copies on one login keep kicking each other off the broker.");
        return;
    }
    wantConnected_ = true;
    consecutiveFailures_ = 0;
    reconnectScheduled_ = false;
    mt5LaunchedThisStart_ = false;
    if (!startRunner(configPath)) {
        wantConnected_ = false;  // bad config: nothing to retry
        instanceLock_.release();
    }
}

bool LiveController::startRunner(const std::string& configPath) {
    if (running_.load()) return true;
    // A previous run may have ended on its own (dropped connection) without
    // anyone calling stop() — join that thread before starting a new one;
    // assigning over a still-joinable std::thread calls std::terminate().
    if (thread_.joinable()) thread_.join();

    clearError();

    LiveConfig config;
    try {
        config = muuk::ctrader_fix::loadLiveConfig(configPath);
    } catch (const std::exception& ex) {
        setError(ex.what());
        return false;
    }

    if (!ensureBridge(config)) return false;
    // MetaTrader 5 isn't connected: start it with the bridge (once per Connect).
    if (config.connection.isMt5() && config.connection.mt5AutoStart && !mt5LaunchedThisStart_ && bridge_ &&
        !bridge_->status().connected) {
        mt5LaunchedThisStart_ = true;
        launchMt5(config);
    }
    attachData(configPath);
    appendLog("---- connecting (" + configPath + ") ----");
    configPath_ = configPath;
    runner_ = std::make_unique<LiveRunner>(std::move(config));
    runner_->setLogHandler([this](const std::string& line) { appendLog(line); });
    if (news_) runner_->setNewsFeed(news_);
    // The running basket survives a disconnect / app restart (see LiveRunner::setStateFile).
    runner_->setStateFile(muuk::ctrader_fix::defaultStateFile(configPath));
    const std::filesystem::path historyFile = muuk::ctrader_fix::historyFileFor(configPath);
    runner_->setBasketHandler([this, historyFile](const muuk::ctrader_fix::BasketRecord& basket) {
        muuk::ctrader_fix::appendBasketHistory(historyFile, basket);  // throws on failure -- LiveRunner logs it
        std::scoped_lock lock(historyMutex_);
        history_.push_back(basket);
        ++historyRevision_;
    });

    running_ = true;
    thread_ = std::thread([this] {
        try {
            runner_->run();
        } catch (const std::exception& ex) {
            appendLog(std::string("[error] ") + ex.what());
            setError(ex.what());
        }
        if (wantConnected_.load()) dropped_ = true;  // ended by itself, not by stop()
        running_ = false;
    });
    return true;
}

void LiveController::tick() {
    const auto now = std::chrono::steady_clock::now();
    if (dropped_.exchange(false)) {
        std::string reason;
        if (const auto snap = snapshot(); snap && !snap->disconnectReason.empty()) reason = snap->disconnectReason;
        if (reason.empty()) reason = lastError();
        if (reason.empty()) reason = "connection ended";
        lastDrop_ = reason;
        lastDropAt_ = muuk::ctrader_fix::localTimestamp();
        if (wantConnected_ && autoReconnect_ && !configPath_.empty()) {
            const int delay = std::min(60, 5 << std::min(consecutiveFailures_, 4));
            ++consecutiveFailures_;
            reconnectAt_ = now + std::chrono::seconds(delay);
            reconnectScheduled_ = true;
            appendLog("[reconnect] connection lost (" + reason + ") -- reconnecting in " + std::to_string(delay) + "s");
        } else {
            appendLog("[reconnect] connection lost (" + reason + ") -- auto-reconnect is off");
        }
    }
    if (reconnectScheduled_ && (!wantConnected_ || !autoReconnect_)) reconnectScheduled_ = false;
    // MetaTrader 5 has just come up: connect now rather than after the back-off.
    if (reconnectScheduled_ && bridge_ && bridge_->status().ready && now + std::chrono::seconds(1) < reconnectAt_) {
        reconnectAt_ = now;
        consecutiveFailures_ = 0;
    }
    if (reconnectScheduled_ && !running_ && now >= reconnectAt_) {
        reconnectScheduled_ = false;
        ++reconnects_;
        appendLog("[reconnect] attempt " + std::to_string(reconnects_));
        if (!startRunner(configPath_)) dropped_ = true;  // try again later
    }
    // A connection that has stayed up for a minute resets the back-off.
    const auto snap = running_ ? snapshot() : std::nullopt;
    if (snap && snap->connected) {
        if (!stableSet_) {
            stableSet_ = true;
            stableSince_ = now;
        } else if (now - stableSince_ > std::chrono::seconds(60)) {
            consecutiveFailures_ = 0;
        }
    } else {
        stableSet_ = false;
    }
}

LiveController::ConnectionHealth LiveController::connectionHealth() const {
    ConnectionHealth h;
    h.reconnects = reconnects_;
    h.lastDrop = lastDrop_;
    h.lastDropAt = lastDropAt_;
    if (reconnectScheduled_) {
        const auto left = std::chrono::ceil<std::chrono::seconds>(reconnectAt_ - std::chrono::steady_clock::now());
        h.reconnectInSeconds = static_cast<int>(std::max<std::chrono::seconds::rep>(0, left.count()));
    }
    return h;
}

void LiveController::stop() {
    wantConnected_ = false;
    reconnectScheduled_ = false;
    const bool wasRunning = runner_ && (running_.load() || thread_.joinable());
    if (runner_) runner_->stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    // Keep a copy of every session's log without anyone having to remember.
    if (wasRunning && !configPath_.empty()) (void)saveLog(configPath_);
    instanceLock_.release();
}

bool LiveController::openRange() { return runner_ && running_.load() && runner_->openRange(); }

void LiveController::closeRange(bool reopen) {
    if (runner_ && running_.load()) runner_->closeRange(reopen);
}

void LiveController::closeAllPositions() {
    if (runner_ && running_.load()) runner_->closeAllPositions();
}

void LiveController::cancelAllPendingOrders() {
    if (runner_ && running_.load()) runner_->cancelAllPendingOrders();
}

void LiveController::closePosition(muuk::core::OrderId id) {
    if (runner_ && running_.load()) runner_->closePosition(id);
}

void LiveController::cancelOrder(muuk::core::OrderId id) {
    if (runner_ && running_.load()) runner_->cancelOrder(id);
}

void LiveController::placeWaitingOrderNow(muuk::core::OrderId id) {
    if (runner_ && running_.load()) runner_->placeWaitingOrderNow(id);
}

void LiveController::syncPositions() {
    if (runner_ && running_.load()) runner_->syncPositions();
}

bool LiveController::flattenAndWait(std::chrono::milliseconds timeout) {
    return runner_ && running_.load() && runner_->flattenAndWait(timeout);
}

void LiveController::applySettings(const LiveSettings& settings) {
    if (runner_ && running_.load()) runner_->applySettings(settings);
}

std::optional<LiveSettings> LiveController::loadSettings(const std::string& configPath) {
    try {
        return muuk::ctrader_fix::settingsFromConfig(muuk::ctrader_fix::loadLiveConfig(configPath));
    } catch (const std::exception& ex) {
        setError(ex.what());
        return std::nullopt;
    }
}

bool LiveController::saveSettings(const std::string& configPath, const LiveSettings& settings) {
    try {
        muuk::ctrader_fix::saveLiveSettings(configPath, settings);
        appendLog("[settings] saved to " + configPath);
        return true;
    } catch (const std::exception& ex) {
        setError(ex.what());
        return false;
    }
}

std::optional<LiveSnapshot> LiveController::snapshot() const {
    if (demo_ && !runner_) return demoSnapshot();
    if (!runner_) return std::nullopt;
    return runner_->snapshot();
}

LiveSnapshot LiveController::demoSnapshot() const {
    using muuk::core::OrderSide;
    using muuk::ctrader_fix::PendingOrderInfo;
    using muuk::ctrader_fix::PositionInfo;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - demoStart_).count();
    LiveSnapshot s;
    s.connected = true;
    s.status = muuk::core::EngineStatus::Active;
    s.environment = "demo";
    s.brokerUid = "preview";
    s.accountLogin = "0000000";
    s.symbolName = "XAUUSD";
    s.tradeHost = "no broker - preview data";
    s.rangeLower = 4271.13;
    s.rangeUpper = 4273.13;
    const double mid = 4272.10 + 1.6 * std::sin(t * 0.35) + 0.35 * std::sin(t * 1.7);
    s.bid = mid - 0.20;
    s.ask = mid + 0.20;
    s.settings.autoTarget = true;
    s.settings.autoTargetPercent = 200;
    s.settings.stopLoss = 30.0;
    s.settings.commissionPerMillion = 30.0;
    s.settings.accountBalance = 1000.0;
    s.settings.leverage = 500.0;
    s.settings.escapeFromLevel = 5;
    s.settings.lockAtLoss = 100.0;
    s.settings.lotMultiplier = 2.0;
    s.settings.rangeWidth = 2.0;
    muuk::core::CostInputs costs;
    costs.commissionPerMillionPerSide = 30.0;
    const muuk::core::SymbolSpec spec;
    uint64_t id = 9001;
    auto position = [&](OrderSide side, double lots, double requested, double fill) {
        const bool buy = side == OrderSide::BuyStop;
        const double oz = lots * spec.contractSize;
        PositionInfo p;
        p.id = id++;
        p.positionId = "P" + std::to_string(p.id);
        p.side = side;
        p.lots = lots;
        p.requestedPrice = requested;
        p.entryPrice = fill;
        p.verified = true;
        p.pnl.gross = (buy ? mid - requested : requested - mid) * oz;
        p.pnl.slippage = (buy ? fill - requested : requested - fill) * oz;
        p.pnl.spread = 0.20 * oz;
        p.pnl.commission = muuk::core::roundTurnCommissionMoney(lots, fill, spec, costs);
        p.pnl.net = p.pnl.gross - p.pnl.slippage - p.pnl.spread - p.pnl.commission;
        p.floatingPnl = p.pnl.net;
        s.live += p.pnl;
        s.positions.push_back(p);
    };
    position(OrderSide::BuyStop, 0.01, 4273.13, 4273.13);
    position(OrderSide::SellStop, 0.02, 4271.13, 4270.85);  // slipped
    position(OrderSide::BuyStop, 0.04, 4273.13, 4273.61);   // slipped
    PendingOrderInfo o{id++, OrderSide::SellStop, 0.08, 4271.13};
    o.confirmed = true;
    s.pendingOrders.push_back(o);
    s.targetNow = muuk::core::autoTargetMoney(s.live.commission + 2.0 * s.live.spread, 200.0);
    {
        double buys = 0.0;
        double sells = 0.0;
        for (const auto& p : s.positions) (p.side == OrderSide::BuyStop ? buys : sells) += p.lots;
        s.equityNow = s.settings.accountBalance + s.live.net;
        s.marginNow = muuk::core::bookMargin(buys, sells, mid, spec.contractSize,
                                             muuk::core::MarginRule{s.settings.leverage, 100.0});
        s.marginLevelNow = muuk::core::marginLevelPercent(s.equityNow, s.marginNow);
    }
    muuk::ctrader_fix::BasketRecord basket;
    basket.number = 1;
    s.currentBasket = basket;
    s.market = muuk::ctrader_fix::evaluateMarket(muuk::ctrader_fix::utcNowMs(), s.settings.market, nullptr, false);
    s.marketKnown = true;
    s.brokerCheckedAt = muuk::ctrader_fix::localTimestamp();
    s.audit = {{"Prices from the broker", 0, "last price 0 s ago"},
               {"Positions match the broker", 0, "matched at " + s.brokerCheckedAt.substr(11)},
               {"Pending order in place", 0, "Sell Stop 0.08 @ 4271.13 (at the broker)"},
               {"Closing orders", 0, "nothing closing"},
               {"Profit target / stop loss", 0, "watching"},
               {"Bot vs its positions", 1, "preview: the range counts buy 0.05 / sell 0.02 lots, its positions differ"},
               {"Daily loss limit", 0, "off"},
               {"Risk settings", 1, "no daily loss limit (Settings)"}};
    s.auditLog = {{"04:10:12", 1, "Closing orders: position 675409669 is already closed at the broker -- booked as closed"},
                  {"04:12:40", 0, "Pending order in place: placed again"}};
    return s;
}

std::vector<std::string> LiveController::logLines() const {
    std::scoped_lock lock(logMutex_);
    return log_;
}

void LiveController::clearLog() {
    std::scoped_lock lock(logMutex_);
    log_.clear();
}

std::string LiveController::lastError() const {
    std::scoped_lock lock(errorMutex_);
    return lastError_;
}

void LiveController::clearError() {
    std::scoped_lock lock(errorMutex_);
    lastError_.clear();
}

void LiveController::appendLog(const std::string& line) {
    std::scoped_lock lock(logMutex_);
    log_.push_back(timestampPrefix() + line);
    if (liveLog_.is_open()) {
        liveLog_ << log_.back() << '\n';
        liveLog_.flush();
    }
    if (log_.size() > kMaxLogLines) {
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - kMaxLogLines));
    }
}

void LiveController::setError(const std::string& message) {
    std::scoped_lock lock(errorMutex_);
    lastError_ = message;
}

std::filesystem::path LiveController::dataDirectory(const std::string& configPath) {
    return muuk::ctrader_fix::dataDirectoryFor(configPath);
}

void LiveController::attachData(const std::string& configPath, bool force) {
    if (!force && dataAttachedFor_ == configPath) return;
    dataAttachedFor_ = configPath;
    reloadHistory(configPath);

    const std::filesystem::path logDir = dataDirectory(configPath) / "muuk_logs";
    const std::filesystem::path liveFile = logDir / "muuk_live.log";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);
    // Keep the running log a manageable size: start a new one past ~20 MB.
    if (std::filesystem::exists(liveFile, ec) && std::filesystem::file_size(liveFile, ec) > 20u * 1024u * 1024u) {
        std::filesystem::rename(liveFile, logDir / ("muuk_live_until_" + fileStamp() + ".log"), ec);
    }

    std::vector<std::string> previous;
    {
        std::ifstream in(liveFile);
        std::string line;
        while (std::getline(in, line)) {
            previous.push_back(std::move(line));
            if (previous.size() > 2 * kMaxLogLines) {
                previous.erase(previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(kMaxLogLines));
            }
        }
    }
    std::scoped_lock lock(logMutex_);
    if (liveLog_.is_open()) liveLog_.close();
    // What's on screen: the end of the saved log, then anything logged
    // before the path was known.
    std::vector<std::string> merged = std::move(previous);
    merged.insert(merged.end(), log_.begin(), log_.end());
    if (merged.size() > kMaxLogLines) {
        merged.erase(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(merged.size() - kMaxLogLines));
    }
    log_ = std::move(merged);
    liveLog_.open(liveFile, std::ios::app);
}

bool LiveController::resetData(const std::string& configPath) {
    const std::string stamp = fileStamp();
    const std::filesystem::path dir = dataDirectory(configPath);
    std::error_code ec;
    try {
        const std::filesystem::path history = muuk::ctrader_fix::historyFileFor(configPath);
        if (std::filesystem::exists(history, ec)) {
            std::filesystem::create_directories(dir / "muuk_history" / "archive");
            std::filesystem::rename(history, dir / "muuk_history" / "archive" /
                                                 (history.stem().string() + "_until_" + stamp + ".jsonl"));
        }
        {
            std::scoped_lock lock(logMutex_);
            if (liveLog_.is_open()) liveLog_.close();
            const std::filesystem::path liveFile = dir / "muuk_logs" / "muuk_live.log";
            if (std::filesystem::exists(liveFile, ec)) {
                std::filesystem::create_directories(dir / "muuk_logs" / "archive");
                std::filesystem::rename(liveFile, dir / "muuk_logs" / "archive" / ("muuk_live_until_" + stamp + ".log"));
            }
            log_.clear();
            std::filesystem::create_directories(liveFile.parent_path(), ec);
            liveLog_.open(liveFile, std::ios::app);
        }
        const std::filesystem::path reports = dir / "muuk_reports";
        if (std::filesystem::exists(reports, ec)) {
            const std::filesystem::path archive = reports / "archive" / ("until_" + stamp);
            std::filesystem::create_directories(archive);
            for (const auto& entry : std::filesystem::directory_iterator(reports)) {
                if (entry.is_regular_file()) std::filesystem::rename(entry.path(), archive / entry.path().filename());
            }
        }
        {
            std::scoped_lock lock(historyMutex_);
            history_.clear();
            historyLoadedFor_ = configPath;
            ++historyRevision_;
        }
        reconnects_ = 0;
        consecutiveFailures_ = 0;
        lastDrop_.clear();
        lastDropAt_.clear();
        clearError();
        if (runner_ && running_.load()) {
            runner_->resetStats();
        } else {
            muuk::ctrader_fix::resetStateStatistics(muuk::ctrader_fix::defaultStateFile(configPath));
            // Not connected: drop the last session's snapshot too, so nothing old stays on screen.
            if (thread_.joinable()) thread_.join();
            runner_.reset();
        }
        appendLog("[reset] fresh start: basket history, log and reports archived (muuk_history/archive, "
                  "muuk_logs/archive, muuk_reports/archive); every statistic starts over now");
        return true;
    } catch (const std::exception& ex) {
        setError(std::string("reset: ") + ex.what());
        return false;
    }
}

std::string LiveController::fileStamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &local);
    return buf;
}

std::vector<muuk::ctrader_fix::BasketRecord> LiveController::history() const {
    std::scoped_lock lock(historyMutex_);
    return history_;
}

void LiveController::reloadHistory(const std::string& configPath) {
    auto loaded = muuk::ctrader_fix::loadBasketHistory(muuk::ctrader_fix::historyFileFor(configPath));
    std::scoped_lock lock(historyMutex_);
    history_ = std::move(loaded);
    historyLoadedFor_ = configPath;
    ++historyRevision_;
}

std::optional<std::filesystem::path> LiveController::exportReport(const std::string& configPath) {
    using namespace muuk::ctrader_fix;
    try {
        {
            std::scoped_lock lock(historyMutex_);
            if (historyLoadedFor_ != configPath) {
                history_ = loadBasketHistory(historyFileFor(configPath));
                historyLoadedFor_ = configPath;
                ++historyRevision_;
            }
        }

        SessionReportInput in;
        in.generatedAt = localTimestamp();
        in.allBaskets = history();
        try {
            if (const auto saved = loadState(defaultStateFile(configPath))) {
                in.statsSince = saved->statsSince;
                in.ticksProcessed = saved->ticksProcessed;
            }
        } catch (const std::exception&) {
            // The report doesn't need it.
        }
        try {
            const LiveConfig config = loadLiveConfig(configPath);
            in.account = config.connection.environment + " " + config.connection.traderLogin;
            in.symbol = config.connection.symbolName;
            in.settings = settingsFromConfig(config);
        } catch (const std::exception&) {
            // Still produce a report from the history alone.
        }
        if (const auto snap = snapshot()) {
            in.settings = snap->settings;  // what the running bot actually uses
            in.sessionStartedAt = snap->sessionStartedAt;
            if (!snap->statsSince.empty()) in.statsSince = snap->statsSince;
            in.tickDecisionUs = snap->tickDecisionUs;
            in.fillReactionUs = snap->fillReactionUs;
            in.brokerRoundTripMs = snap->brokerRoundTripMs;
            in.ticksProcessed = snap->ticksProcessed;
            in.sessionBaskets = snap->baskets;
            in.openPositions = static_cast<int>(snap->positions.size());
            in.openNet = snap->live.net;
        }

        const std::filesystem::path file =
            dataDirectory(configPath) / "muuk_reports" / ("muuk_report_" + fileStamp() + ".pdf");
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + file.string());
        const std::string pdf = buildSessionReportPdf(in);
        out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
        appendLog("[report] saved " + file.string());
        return file;
    } catch (const std::exception& ex) {
        setError(std::string("report: ") + ex.what());
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> LiveController::saveLog(const std::string& configPath) {
    const std::vector<std::string> lines = logLines();
    try {
        const std::filesystem::path file = dataDirectory(configPath) / "muuk_logs" / ("muuk_" + fileStamp() + ".log");
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + file.string());
        for (const auto& l : lines) out << l << "\n";
        return file;
    } catch (const std::exception& ex) {
        setError(std::string("log: ") + ex.what());
        return std::nullopt;
    }
}

}  // namespace muuk::desktop
