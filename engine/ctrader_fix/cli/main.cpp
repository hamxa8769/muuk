#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"

namespace {
muuk::ctrader_fix::LiveRunner* g_runner = nullptr;
bool g_flattenOnExit = true;
bool g_shutdownStarted = false;
std::mutex g_runnerMutex;  // keeps main() from destroying the runner while a handler still uses it

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
            shutdownRunner(std::chrono::seconds(20));
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--credentials" && i + 1 < argc) {
            credentialsPath = argv[++i];
        } else if (arg == "--keep-open-on-exit") {
            g_flattenOnExit = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: muuk-live --credentials <local-config.json> [--keep-open-on-exit]\n"
                         "  (see README.md / docs/ctrader-fix.md for the config file's shape;\n"
                         "   keep this file out of git — it holds your cTrader FIX login.)\n"
                         "  On Ctrl+C, closing the console, logoff or shutdown every open position is\n"
                         "  closed and every pending order cancelled first, unless --keep-open-on-exit.\n";
            return 0;
        }
    }

    if (credentialsPath.empty()) {
        std::cerr << "usage: muuk-live --credentials <local-config.json> [--keep-open-on-exit]\n";
        return 1;
    }

    try {
        const muuk::ctrader_fix::LiveConfig config = muuk::ctrader_fix::loadLiveConfig(credentialsPath);
        muuk::ctrader_fix::LiveRunner runner(config);
        const RunnerRegistration registration(&runner);

#ifdef _WIN32
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        runner.run();
#else
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        std::atomic<bool> done{false};
        std::thread watcher([&] {
            while (!done && !g_signalled) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (g_signalled) shutdownRunner(std::chrono::seconds(20));
        });
        try {
            runner.run();
        } catch (...) {
            done = true;
            watcher.join();
            throw;
        }
        done = true;
        watcher.join();
#endif
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
