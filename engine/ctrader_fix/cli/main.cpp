#include <csignal>
#include <iostream>
#include <string>

#include "muuk/ctrader_fix/live_config.hpp"
#include "muuk/ctrader_fix/live_runner.hpp"

namespace {
muuk::ctrader_fix::LiveRunner* g_runner = nullptr;

void handleSignal(int) {
    if (g_runner) g_runner->stop();
}
}  // namespace

int main(int argc, char** argv) {
    std::string credentialsPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--credentials" && i + 1 < argc) {
            credentialsPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: muuk-live --credentials <local-config.json>\n"
                         "  (see README.md / docs/ctrader-fix.md for the config file's shape;\n"
                         "   keep this file out of git — it holds your cTrader FIX login.)\n";
            return 0;
        }
    }

    if (credentialsPath.empty()) {
        std::cerr << "usage: muuk-live --credentials <local-config.json>\n";
        return 1;
    }

    try {
        const muuk::ctrader_fix::LiveConfig config = muuk::ctrader_fix::loadLiveConfig(credentialsPath);
        muuk::ctrader_fix::LiveRunner runner(config);

        g_runner = &runner;
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        runner.run();
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
