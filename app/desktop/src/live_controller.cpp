#include "live_controller.hpp"

#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <stdexcept>

#include "muuk/ctrader_fix/basket_history.hpp"
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
        return;
    }

    appendLog("---- connecting (" + configPath + ") ----");
    configPath_ = configPath;
    reloadHistory(configPath);
    runner_ = std::make_unique<LiveRunner>(std::move(config));
    runner_->setLogHandler([this](const std::string& line) { appendLog(line); });
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
        running_ = false;
    });
}

void LiveController::stop() {
    const bool wasRunning = runner_ && (running_.load() || thread_.joinable());
    if (runner_) runner_->stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
    // Keep a copy of every session's log without anyone having to remember.
    if (wasRunning && !configPath_.empty()) (void)saveLog(configPath_);
}

bool LiveController::openRange() { return runner_ && running_.load() && runner_->openRange(); }

void LiveController::closeRange() {
    if (runner_ && running_.load()) runner_->closeRange();
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
    if (log_.size() > kMaxLogLines) {
        log_.erase(log_.begin(), log_.begin() + static_cast<std::ptrdiff_t>(log_.size() - kMaxLogLines));
    }
}

void LiveController::setError(const std::string& message) {
    std::scoped_lock lock(errorMutex_);
    lastError_ = message;
}

std::filesystem::path LiveController::dataDirectory(const std::string& configPath) {
    std::error_code ec;
    const std::filesystem::path config(configPath);
    if (!configPath.empty() && config.has_parent_path()) return std::filesystem::absolute(config.parent_path(), ec);
    return std::filesystem::current_path(ec);
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
