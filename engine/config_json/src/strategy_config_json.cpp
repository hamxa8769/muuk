#include "muuk/config_json/strategy_config_json.hpp"

#include <stdexcept>

using namespace muuk::core;

namespace muuk::config_json {

SymbolSpec parseSymbolSpec(const nlohmann::json& j) {
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

StrategyConfig parseStrategyConfig(const nlohmann::json& j) {
    StrategyConfig cfg;
    const SymbolSpec spec = parseSymbolSpec(j.at("symbol"));
    cfg.symbol = spec;

    const auto& range = j.at("range");
    cfg.range = RangeConfig{Price::fromDouble(range.at("lower").get<double>(), spec),
                             Price::fromDouble(range.at("upper").get<double>(), spec)};

    const auto& lots = j.at("lotProgression");
    cfg.lotProgression = LotProgressionConfig{Volume::fromDouble(lots.at("initialLot").get<double>(), spec),
                                               Volume::fromDouble(lots.at("lotIncrement").get<double>(), spec),
                                               lots.value("lotMultiplier", 1.0)};

    if (j.contains("riskLimits")) {
        const auto& risk = j.at("riskLimits");
        cfg.riskLimits.maxOrders = risk.value("maxOrders", cfg.riskLimits.maxOrders);
        cfg.riskLimits.maxLot = Volume::fromDouble(risk.value("maxLot", 100.0), spec);
        cfg.riskLimits.maxBasketLossMoney = risk.value("maxBasketLossMoney", cfg.riskLimits.maxBasketLossMoney);
        cfg.riskLimits.maxDrawdownMoney = risk.value("maxDrawdownMoney", cfg.riskLimits.maxDrawdownMoney);
        cfg.riskLimits.maxMarginUsageFraction =
            risk.value("maxMarginUsageFraction", cfg.riskLimits.maxMarginUsageFraction);
        cfg.riskLimits.maxSpreadTicks = risk.value("maxSpreadTicks", cfg.riskLimits.maxSpreadTicks);
        cfg.riskLimits.maxSlippageTicks = risk.value("maxSlippageTicks", cfg.riskLimits.maxSlippageTicks);
    }

    if (j.contains("costs")) {
        const auto& costs = j.at("costs");
        cfg.costs.commissionPerLotRoundTurn =
            costs.value("commissionPerLotRoundTurn", cfg.costs.commissionPerLotRoundTurn);
        cfg.costs.commissionPerMillionPerSide =
            costs.value("commissionPerMillionPerSide", cfg.costs.commissionPerMillionPerSide);
        cfg.costs.slippageBufferTicks = costs.value("slippageBufferTicks", cfg.costs.slippageBufferTicks);
    }

    cfg.basketProfitTargetMoney = j.value("basketProfitTargetMoney", 0.0);
    cfg.basketStopLossMoney = j.value("basketStopLossMoney", 0.0);
    if (j.contains("autoProfitTarget") && j.at("autoProfitTarget").is_object()) {
        const auto& at = j.at("autoProfitTarget");
        cfg.autoProfitTarget = at.value("enabled", cfg.autoProfitTarget);
        cfg.autoTargetPercent = at.value("percent", cfg.autoTargetPercent);
    }
    cfg.maxRejectRetries = j.value("maxRejectRetries", cfg.maxRejectRetries);
    cfg.reArmOnFill = j.value("reArmOnFill", cfg.reArmOnFill);
    cfg.seedOffsetPoints = j.value("seedOffsetPoints", cfg.seedOffsetPoints);
    cfg.startSide = parseStartSide(j.value("startSide", std::string("both")));

    return cfg;
}

StartSide parseStartSide(const std::string& value) {
    if (value == "buy") return StartSide::BuyOnly;
    if (value == "sell") return StartSide::SellOnly;
    if (value == "both") return StartSide::Both;
    throw std::runtime_error("strategy.startSide must be \"buy\", \"sell\" or \"both\" (got \"" + value + "\")");
}

std::string startSideToString(StartSide side) {
    switch (side) {
        case StartSide::BuyOnly:
            return "buy";
        case StartSide::SellOnly:
            return "sell";
        case StartSide::Both:
        default:
            return "both";
    }
}

}  // namespace muuk::config_json
