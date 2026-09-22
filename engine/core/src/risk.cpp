#include "muuk/core/risk.hpp"

namespace muuk::core {

RiskLimitKind evaluateRisk(const RiskLimits& limits, const RiskCheckInput& input) {
    if (limits.maxOrders > 0 && input.activeOrderCount >= limits.maxOrders) {
        return RiskLimitKind::MaxOrders;
    }
    if (limits.maxLot.steps > 0 && input.nextOrderVolume.steps > limits.maxLot.steps) {
        return RiskLimitKind::MaxLot;
    }
    if (limits.maxBasketLossMoney > 0.0 && input.basketLossMoney >= limits.maxBasketLossMoney) {
        return RiskLimitKind::MaxBasketLoss;
    }
    if (limits.maxDrawdownMoney > 0.0 && input.drawdownMoney >= limits.maxDrawdownMoney) {
        return RiskLimitKind::MaxDrawdown;
    }
    if (limits.maxMarginUsageFraction > 0.0 &&
        input.marginUsageFraction >= limits.maxMarginUsageFraction) {
        return RiskLimitKind::MaxMargin;
    }
    if (limits.maxSpreadTicks > 0 && input.currentSpreadTicks > limits.maxSpreadTicks) {
        return RiskLimitKind::MaxSpread;
    }
    if (limits.maxSlippageTicks > 0 && input.estimatedSlippageTicks > limits.maxSlippageTicks) {
        return RiskLimitKind::MaxSlippage;
    }
    return RiskLimitKind::None;
}

}  // namespace muuk::core
