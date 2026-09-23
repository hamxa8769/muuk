#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "muuk/backtester/backtest_runner.hpp"
#include "muuk/backtester/tick_data_source.hpp"
#include "muuk/config_json/strategy_config_json.hpp"

using namespace muuk::core;
using namespace muuk::backtester;
using json = nlohmann::json;

namespace {

BacktestConfig parseBacktestConfig(const json& j) {
    BacktestConfig cfg;
    cfg.strategy = muuk::config_json::parseStrategyConfig(j);

    if (j.contains("fillSim")) {
        cfg.fillSim.slippageBufferTicks = j.at("fillSim").value("slippageBufferTicks", 0.0);
    }

    return cfg;
}

void printReport(const BacktestReport& report) {
    std::cout << "Ticks processed:      " << report.ticksProcessed << "\n";
    std::cout << "Orders placed:        " << report.ordersPlaced << "\n";
    std::cout << "Orders filled:        " << report.ordersFilled << "\n";
    std::cout << "Baskets closed:       " << report.basketsClosed << "\n";
    std::cout << "Risk halts:           " << report.riskHalts << "\n";
    std::cout << "Net P/L (money):      " << report.netPnlMoney << "\n";
    std::cout << "Max drawdown (money): " << report.maxDrawdownMoney << "\n";
    std::cout << "Worst basket (money): " << report.worstBasketLossMoney << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string dataPath;
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) {
            dataPath = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: muuk-backtest --data <ticks.csv> --config <strategy.json>\n";
            return 0;
        }
    }

    if (dataPath.empty() || configPath.empty()) {
        std::cerr << "usage: muuk-backtest --data <ticks.csv> --config <strategy.json>\n";
        return 1;
    }

    try {
        std::ifstream configFile(configPath);
        if (!configFile) {
            std::cerr << "cannot open config file: " << configPath << "\n";
            return 1;
        }
        json configJson;
        configFile >> configJson;

        const BacktestConfig config = parseBacktestConfig(configJson);
        const std::vector<TickRecord> ticks = TickDataSource::loadCsv(dataPath);

        BacktestRunner runner(config);
        const BacktestReport report = runner.run(ticks);
        printReport(report);
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
