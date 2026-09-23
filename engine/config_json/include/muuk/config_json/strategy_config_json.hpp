#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "muuk/core/strategy_engine.hpp"

namespace muuk::config_json {

// Shared JSON schema for SymbolSpec/StrategyConfig, documented in
// docs/data-format.md. Used by both the backtester CLI and the live
// (ctrader_fix) CLI so the two never drift out of sync.
[[nodiscard]] muuk::core::SymbolSpec parseSymbolSpec(const nlohmann::json& j);
[[nodiscard]] muuk::core::StrategyConfig parseStrategyConfig(const nlohmann::json& j);

// strategy.startSide: "buy" | "sell" | "both" (default "both"). Throws
// std::runtime_error on anything else.
[[nodiscard]] muuk::core::StartSide parseStartSide(const std::string& value);
[[nodiscard]] std::string startSideToString(muuk::core::StartSide side);

}  // namespace muuk::config_json
