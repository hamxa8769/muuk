#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "muuk/ctrader_fix/instance_lock.hpp"
#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"
#include "muuk/ctrader_fix/live_state.hpp"
#include "muuk/mt5_bridge/mt5_gateway.hpp"

namespace {
muuk::ctrader_fix::LiveRunner* g_runner = nullptr;
bool g_flattenOnExit = true;
bool g_shutdownStarted = false;
std::mutex g_runnerMutex;  // keeps main() from destroying the runner while a handler still uses it
std::atomic<bool> g_exitRequested{false};  // Ctrl+C / close / logoff / shutdown: stop, don't reconnect

// Exit guard: unless --keep-open-on-exit was given, close every position and
// cancel every pending order before disconnecting, so the account is never
// left with trades nobody is managing. Runs at most once.
void shutdownRunner(std::chrono::milliseconds flattenTimeout) {
    std::scoped_lock lock(g_runnerMutex);
    if (!g_runner || g_shutdownStarted) return;
    g_shutdownStarted = true;
    if (g_flattenOnExit) (void)g_runner->flattenAndWait(flattenTimeout);
    g_runner->stop();
}

// Publishes the runner to the handlers for exactly its lifetime -- also when
// run() throws. The destructor waits for a handler that's mid-shutdown.
struct RunnerRegistration {
    explicit RunnerRegistration(muuk::ctrader_fix::LiveRunner* runner) {
        std::scoped_lock lock(g_runnerMutex);
        g_runner = runner;
    }
    ~RunnerRegistration() {
        std::scoped_lock lock(g_runnerMutex);
        g_runner = nullptr;
    }
    RunnerRegistration(const RunnerRegistration&) = delete;
    RunnerRegistration& operator=(const RunnerRegistration&) = delete;
};

#ifdef _WIN32
// Runs on its own thread, and Windows waits for it: Ctrl+C / Ctrl+Break
// without a limit, closing the console window ~5 s, logoff/shutdown ~5-20 s
// -- hence the shorter budget for those.
BOOL WINAPI consoleHandler(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            g_exitRequested = true;
            shutdownRunner(std::chrono::seconds(20));
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_exitRequested = true;
            shutdownRunner(std::chrono::milliseconds(4500));
            return TRUE;
        default:
            return FALSE;
    }
}
#else
std::atomic<bool> g_signalled{false};
void handleSignal(int) { g_signalled = true; }  // async-signal-safe; the watcher thread does the work
#endif
}  // namespace

int main(int argc, char** argv) {
    std::string credentialsPath;
    bool reconnect = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--credentials" && i + 1 < argc) {
            credentialsPath = argv[++i];
        } else if (arg == "--keep-open-on-exit") {
            g_flattenOnExit = false;
        } else if (arg == "--no-reconnect") {
            reconnect = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: muuk-live --credentials <local-config.json> [--keep-open-on-exit] [--no-reconnect]\n"
                         "  (see README.md / docs/ctrader-fix.md for the config file's shape;\n"
                         "   keep this file out of git — it holds your cTrader FIX login.)\n"
                         "  On Ctrl+C, closing the console, logoff or shutdown every open position is\n"
                         "  closed and every pending order cancelled first, unless --keep-open-on-exit.\n"
                         "  A dropped connection is retried after 5, 10, 20, 40, then every 60 s, and\n"
                         "  the running basket is picked up again -- unless --no-reconnect.\n";
            return 0;
        }
    }

    if (credentialsPath.empty()) {
        std::cerr << "usage: muuk-live --credentials <local-config.json> [--keep-open-on-exit]\n";
        return 1;
    }

    muuk::ctrader_fix::InstanceLock instanceLock;
    if (!instanceLock.acquire(muuk::ctrader_fix::instanceLockFile(credentialsPath))) {
        std::cerr << "error: Muuk is already running with this config (the app, another muuk-live, or the auto-start "
                     "task). Close that one first -- two copies on one login keep kicking each other off the broker.\n";
        return 2;
    }

    // A bad config is a mistake to fix, not something to retry.
    std::unique_ptr<muuk::mt5_bridge::Mt5Gateway> bridge;
    try {
        const auto config = muuk::ctrader_fix::loadLiveConfig(credentialsPath);
        if (config.connection.isMt5()) {
            // MetaTrader 5: the bridge stays up for the whole run, across reconnects.
            muuk::mt5_bridge::GatewayConfig gc;
            gc.eaPort = static_cast<uint16_t>(config.connection.mt5EaPort);
            gc.fixPort = static_cast<uint16_t>(config.connection.mt5FixPort);
            bridge = std::make_unique<muuk::mt5_bridge::Mt5Gateway>(
                gc, [](const std::string& line) { std::cerr << line << "\n"; });
            bridge->start();
        }
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::atomic<bool> done{false};
    std::thread watcher([&] {
        while (!done && !g_signalled) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (g_signalled) {
            g_exitRequested = true;
            shutdownRunner(std::chrono::seconds(20));
        }
    });
#endif

    // The running basket is saved next to the config and picked up again on
    // every (re)connect (see LiveRunner::setStateFile).
    const auto stateFile = muuk::ctrader_fix::defaultStateFile(credentialsPath);
    std::cerr << "state file: " << stateFile.string() << "\n";
    // ForexFactory calendar for the news guard, cached next to the state file.
    const auto news = std::make_shared<muuk::ctrader_fix::NewsFeed>(stateFile.parent_path() / "news_calendar.json");
    news->start();
    int failures = 0;
    while (!g_exitRequested) {
        const auto startedAt = std::chrono::steady_clock::now();
        try {
            const muuk::ctrader_fix::LiveConfig config = muuk::ctrader_fix::loadLiveConfig(credentialsPath);
            muuk::ctrader_fix::LiveRunner runner(config);
            runner.setStateFile(stateFile);
            runner.setNewsFeed(news);
            const RunnerRegistration registration(&runner);
            {
                std::scoped_lock lock(g_runnerMutex);
                g_shutdownStarted = false;  // this runner can be shut down
            }
            if (!g_exitRequested) runner.run();
        } catch (const std::exception& ex) {
            std::cerr << "error: " << ex.what() << "\n";
        }
        if (g_exitRequested || !reconnect) break;

        // Back off: 5, 10, 20, 40, then 60 s -- reset once a connection held for a minute.
        if (std::chrono::steady_clock::now() - startedAt > std::chrono::seconds(60)) failures = 0;
        const int delay = std::min(60, 5 << std::min(failures, 4));
        ++failures;
        std::cerr << "[reconnect] connection lost -- reconnecting in " << delay << "s\n";
        for (int i = 0; i < delay * 10 && !g_exitRequested; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#ifndef _WIN32
    done = true;
    watcher.join();
#endif
    return 0;
}
