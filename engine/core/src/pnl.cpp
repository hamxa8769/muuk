#include "muuk/core/pnl.hpp"

namespace muuk::core {

double positionPnlMoney(OrderSide side,
                         Volume volume,
                         Price entryPrice,
                         Price markPrice,
                         const SymbolSpec& spec) {
    const int64_t tickDelta = (side == OrderSide::BuyStop)
                                   ? (markPrice.ticks - entryPrice.ticks)
                                   : (entryPrice.ticks - markPrice.ticks);
    const double lots = volume.toDouble(spec);
    return static_cast<double>(tickDelta) * spec.tickValue * lots;
}

double aggregatePositionPnlMoney(OrderSide side,
                                  int64_t totalVolumeSteps,
                                  int64_t sumEntryPriceTicksVolume,
                                  Price markPrice,
                                  const SymbolSpec& spec) {
    const int64_t tickWeightedDelta = (side == OrderSide::BuyStop)
                                           ? (markPrice.ticks * totalVolumeSteps - sumEntryPriceTicksVolume)
                                           : (sumEntryPriceTicksVolume - markPrice.ticks * totalVolumeSteps);
    return static_cast<double>(tickWeightedDelta) * spec.tickValue * spec.volumeStep;
}

BasketPnlResult computeBasketPnl(const BasketPnlInput& input, const CostInputs& costs) {
    BasketPnlResult result;
    result.grossPnlMoney = input.grossPnlMoney;
    result.commissionMoney = input.totalVolumeLots * costs.commissionPerLotRoundTurn;
    result.spreadCostMoney = input.spreadCostMoney;
    result.swapMoney = input.swapMoney;
    result.slippageMoney = input.slippageMoney;
    result.netPnlMoney = result.grossPnlMoney - result.commissionMoney -
                          result.spreadCostMoney - result.swapMoney - result.slippageMoney;
    return result;
}

}  // namespace muuk::core
