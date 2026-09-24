#include "live_controller.hpp"

#include <chrono>
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

LiveController::~LiveController() { stop(); }

void LiveController::start(const std::string& configPath) {
    if (running_.load()) return;
    if (!instanceLock_.acquire(muuk::ctrader_fix::instanceLockFile(configPath))) {
        setError("Muuk is already running with this config (another window, muuk-live, or the auto-start task). "
                 "Close that one first -- two copies on one login keep kicking each other off the broker.");
        return;
    }
    wantConnected_ = true;
    consecutiveFailures_ = 0;
    reconnectScheduled_ = false;
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

    attachData(configPath);
    appendLog("---- connecting (" + configPath + ") ----");
    configPath_ = configPath;
    runner_ = std::make_unique<LiveRunner>(std::move(config));
    runner_->setLogHandler([this](const std::string& line) { appendLog(line); });
    // The running basket survives a disconnect / app restart (see LiveRunner::setStateFile).
    runner_->setStateFile(muuk::ctrader_fix::defaultStateFile(configPath));
    const std::filesystem::path historyFile = dataDirectory(configPath) / "muuk_history" / "baskets.jsonl";
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
    if (!runner_) return std::nullopt;
    return runner_->snapshot();
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
        const std::filesystem::path history = dir / "muuk_history" / "baskets.jsonl";
        if (std::filesystem::exists(history, ec)) {
            std::filesystem::create_directories(dir / "muuk_history" / "archive");
            std::filesystem::rename(history, dir / "muuk_history" / "archive" / ("baskets_until_" + stamp + ".jsonl"));
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
        {
            std::scoped_lock lock(historyMutex_);
            history_.clear();
            historyLoadedFor_ = configPath;
            ++historyRevision_;
        }
        if (runner_ && running_.load()) {
            runner_->resetStats();
        } else {
            muuk::ctrader_fix::resetStateStatistics(muuk::ctrader_fix::defaultStateFile(configPath));
        }
        appendLog("[reset] fresh start: basket history and log archived under muuk_history/archive and "
                  "muuk_logs/archive; statistics start over now");
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
    auto loaded = muuk::ctrader_fix::loadBasketHistory(dataDirectory(configPath) / "muuk_history" / "baskets.jsonl");
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
                history_ = loadBasketHistory(dataDirectory(configPath) / "muuk_history" / "baskets.jsonl");
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
