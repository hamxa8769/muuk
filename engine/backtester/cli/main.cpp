#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "muuk/backtester/backtest_runner.hpp"
#include "muuk/backtester/tick_data_source.hpp"

using namespace muuk::core;
using namespace muuk::backtester;
using json = nlohmann::json;

namespace {

SymbolSpec parseSymbolSpec(const json& j) {
    SymbolSpec spec;
    spec.name = j.value("name", spec.name);
    spec.digits = j.value("digits", spec.digits);
    spec.tickSize = j.value("tickSize", spec.tickSize);
    spec.tickValue = j.value("tickValue", spec.tickValue);
    spec.contractSize = j.value("contractSize", spec.contractSize);
    spec.volumeStep = j.value("volumeStep", spec.volumeStep);
    spec.volumeMin = j.value("volumeMin", spec.volumeMin);
    spec.volumeMax = j.value("volumeMax", spec.volumeMax);
    return spec;
}

BacktestConfig parseBacktestConfig(const json& j) {
    BacktestConfig cfg;
    const SymbolSpec spec = parseSymbolSpec(j.at("symbol"));
    cfg.strategy.symbol = spec;

    const auto& range = j.at("range");
    cfg.strategy.range = RangeConfig{Price::fromDouble(range.at("lower").get<double>(), spec),
                                      Price::fromDouble(range.at("upper").get<double>(), spec)};

    const auto& lots = j.at("lotProgression");
    cfg.strategy.lotProgression =
        LotProgressionConfig{Volume::fromDouble(lots.at("initialLot").get<double>(), spec),
                              Volume::fromDouble(lots.at("lotIncrement").get<double>(), spec)};

    if (j.contains("riskLimits")) {
        const auto& risk = j.at("riskLimits");
        cfg.strategy.riskLimits.maxOrders = risk.value("maxOrders", cfg.strategy.riskLimits.maxOrders);
        cfg.strategy.riskLimits.maxLot =
            Volume::fromDouble(risk.value("maxLot", 100.0), spec);
        cfg.strategy.riskLimits.maxBasketLossMoney =
            risk.value("maxBasketLossMoney", cfg.strategy.riskLimits.maxBasketLossMoney);
        cfg.strategy.riskLimits.maxDrawdownMoney =
            risk.value("maxDrawdownMoney", cfg.strategy.riskLimits.maxDrawdownMoney);
        cfg.strategy.riskLimits.maxMarginUsageFraction =
            risk.value("maxMarginUsageFraction", cfg.strategy.riskLimits.maxMarginUsageFraction);
        cfg.strategy.riskLimits.maxSpreadTicks =
            risk.value("maxSpreadTicks", cfg.strategy.riskLimits.maxSpreadTicks);
        cfg.strategy.riskLimits.maxSlippageTicks =
            risk.value("maxSlippageTicks", cfg.strategy.riskLimits.maxSlippageTicks);
    }

    if (j.contains("costs")) {
        const auto& costs = j.at("costs");
        cfg.strategy.costs.commissionPerLotRoundTurn =
            costs.value("commissionPerLotRoundTurn", cfg.strategy.costs.commissionPerLotRoundTurn);
        cfg.strategy.costs.slippageBufferTicks =
            costs.value("slippageBufferTicks", cfg.strategy.costs.slippageBufferTicks);
    }

    cfg.strategy.basketProfitTargetMoney = j.value("basketProfitTargetMoney", 0.0);
    cfg.strategy.maxRejectRetries = j.value("maxRejectRetries", cfg.strategy.maxRejectRetries);

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
