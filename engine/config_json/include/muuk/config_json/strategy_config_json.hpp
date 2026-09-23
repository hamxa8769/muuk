#pragma once

#include <nlohmann/json.hpp>

#include "muuk/core/strategy_engine.hpp"

namespace muuk::config_json {

// Shared JSON schema for SymbolSpec/StrategyConfig, documented in
// docs/data-format.md. Used by both the backtester CLI and the live
// (ctrader_fix) CLI so the two never drift out of sync.
[[nodiscard]] muuk::core::SymbolSpec parseSymbolSpec(const nlohmann::json& j);
[[nodiscard]] muuk::core::StrategyConfig parseStrategyConfig(const nlohmann::json& j);

}  // namespace muuk::config_json
