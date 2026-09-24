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

double roundTurnCommissionMoney(double lots, double entryPrice, const SymbolSpec& spec, const CostInputs& costs) {
    if (costs.commissionPerMillionPerSide > 0.0) {
        const double oneSide = lots * spec.contractSize * entryPrice * costs.commissionPerMillionPerSide / 1'000'000.0;
        // The broker keeps money in whole cents; + a hair so 4.105 isn't read as 4.10499999.
        const double cents = static_cast<double>(detail::roundToInt64(oneSide * 100.0 + 1e-7));
        return 2.0 * cents / 100.0;
    }
    return lots * costs.commissionPerLotRoundTurn;
}

double autoTargetMoney(double costMoney, double percent) {
    const double p = percent < 1.0 ? 1.0 : (percent > 1000.0 ? 1000.0 : percent);
    const double target = costMoney * p / 100.0;
    return target > 0.01 ? target : 0.01;
}

BasketPnlResult computeBasketPnl(const BasketPnlInput& input, const CostInputs& costs) {
    BasketPnlResult result;
    result.grossPnlMoney = input.grossPnlMoney;
    result.commissionMoney = input.commissionMoney ? *input.commissionMoney
                                                   : input.totalVolumeLots * costs.commissionPerLotRoundTurn;
    result.spreadCostMoney = input.spreadCostMoney;
    result.swapMoney = input.swapMoney;
    result.slippageMoney = input.slippageMoney;
    result.netPnlMoney = result.grossPnlMoney - result.commissionMoney -
                          result.spreadCostMoney - result.swapMoney - result.slippageMoney;
    return result;
}

}  // namespace muuk::core
