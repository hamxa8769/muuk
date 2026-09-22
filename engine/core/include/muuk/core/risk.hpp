#pragma once

#include <cstdint>

#include "muuk/core/types.hpp"

namespace muuk::core {

// Hard limits the risk engine enforces. Proposal §8: unlimited order growth
// must not be allowed. Any one of these being exceeded halts new order
// creation and raises an alert.
struct RiskLimits {
    int maxOrders = 10;
    Volume maxLot;                  // largest single order allowed
    double maxBasketLossMoney = 0.0;  // hard stop on floating/realized loss
    double maxDrawdownMoney = 0.0;
    double maxMarginUsageFraction = 0.0; // e.g. 0.8 == 80% of available margin
    int64_t maxSpreadTicks = 0;
    int64_t maxSlippageTicks = 0;
};

enum class RiskLimitKind {
    None,
    MaxOrders,
    MaxLot,
    MaxBasketLoss,
    MaxDrawdown,
    MaxMargin,
    MaxSpread,
    MaxSlippage,
};

// Snapshot of the values the risk engine evaluates against `RiskLimits`.
// Built by the caller (StrategyEngine) from its current state each time a
// risk check is needed — no I/O, no allocation.
struct RiskCheckInput {
    int activeOrderCount = 0;
    Volume nextOrderVolume;
    double basketLossMoney = 0.0;   // positive == currently losing this much
    double drawdownMoney = 0.0;
    double marginUsageFraction = 0.0;
    int64_t currentSpreadTicks = 0;
    int64_t estimatedSlippageTicks = 0;
};

// Evaluates `input` against `limits` and returns the first breached limit,
// or RiskLimitKind::None if everything is within bounds. Check order is
// fixed so results are deterministic and reproducible in backtests.
[[nodiscard]] RiskLimitKind evaluateRisk(const RiskLimits& limits,
                                          const RiskCheckInput& input);

}  // namespace muuk::core
