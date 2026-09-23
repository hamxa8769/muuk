#include "live_controller.hpp"

#include "muuk/ctrader_fix/live_config.hpp"

namespace muuk::desktop {

using muuk::ctrader_fix::LiveConfig;
using muuk::ctrader_fix::LiveRunner;
using muuk::ctrader_fix::LiveSnapshot;

LiveController::~LiveController() { stop(); }

void LiveController::start(const std::string& configPath) {
    if (running_.load()) return;
    // A previous run may have ended on its own (dropped connection) without
    // anyone calling stop() — join that thread before starting a new one;
    // assigning over a still-joinable std::thread calls std::terminate().
    if (thread_.joinable()) thread_.join();

    {
        std::scoped_lock lock(errorMutex_);
        lastError_.clear();
    }
    {
        std::scoped_lock lock(logMutex_);
        log_.clear();
    }

    LiveConfig config;
    try {
        config = muuk::ctrader_fix::loadLiveConfig(configPath);
    } catch (const std::exception& ex) {
        std::scoped_lock lock(errorMutex_);
        lastError_ = ex.what();
        return;
    }

    runner_ = std::make_unique<LiveRunner>(std::move(config));
    runner_->setLogHandler([this](const std::string& line) { appendLog(line); });

    running_ = true;
    thread_ = std::thread([this] {
        try {
            runner_->run();
        } catch (const std::exception& ex) {
            appendLog(std::string("[error] ") + ex.what());
            std::scoped_lock lock(errorMutex_);
            lastError_ = ex.what();
        }
        running_ = false;
    });
}

void LiveController::stop() {
    if (runner_) runner_->stop();
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void LiveController::closeAllPositions() {
    if (runner_ && running_.load()) runner_->closeAllPositions();
}

void LiveController::cancelAllPendingOrders() {
    if (runner_ && running_.load()) runner_->cancelAllPendingOrders();
}

std::optional<LiveSnapshot> LiveController::snapshot() const {
    if (!runner_) return std::nullopt;
    return runner_->snapshot();
}

std::vector<std::string> LiveController::logLines() const {
    std::scoped_lock lock(logMutex_);
    return log_;
}

std::string LiveController::lastError() const {
    std::scoped_lock lock(errorMutex_);
    return lastError_;
}

void LiveController::appendLog(const std::string& line) {
    std::scoped_lock lock(logMutex_);
    log_.push_back(line);
    if (log_.size() > kMaxLogLines) {
        log_.erase(log_.begin());
    }
}

}  // namespace muuk::desktop
