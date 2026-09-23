#include "live_controller.hpp"

#include <chrono>
#include <ctime>
#include <exception>

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
    runner_ = std::make_unique<LiveRunner>(std::move(config));
    runner_->setLogHandler([this](const std::string& line) { appendLog(line); });

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
    if (runner_) runner_->stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
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

}  // namespace muuk::desktop
