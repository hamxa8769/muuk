#pragma once

#include <cstdint>

#include "muuk/core/orders.hpp"
#include "muuk/core/types.hpp"

namespace muuk::core {

// Cost model inputs used to turn a raw price move into money and to derive
// the deductions in the basket P/L formula. Proposal §6/§7.
struct CostInputs {
    double commissionPerLotRoundTurn = 0.0; // money, charged per 1.0 lot traded
    double slippageBufferTicks = 0.0;       // conservative pre-trade estimate
};

// Gross P/L (in account money) for a single filled position between its
// entry and a mark price (current price for floating, exit price for
// realized), using SymbolSpec::tickValue scaled by volume in lots.
[[nodiscard]] double positionPnlMoney(OrderSide side,
                                       Volume volume,
                                       Price entryPrice,
                                       Price markPrice,
                                       const SymbolSpec& spec);

// Gross P/L for many same-side fills, without retaining each fill
// individually — mathematically equivalent to summing positionPnlMoney()
// over every fill, but O(1) in the number of fills. The caller maintains
// two running sums per side as fills arrive:
//   totalVolumeSteps         += volume.steps                    (per fill)
//   sumEntryPriceTicksVolume += entryPrice.ticks * volume.steps  (per fill)
// This is what lets StrategyEngine track basket P/L with fixed-size state
// no matter how many times a side has re-armed and filled.
[[nodiscard]] double aggregatePositionPnlMoney(OrderSide side,
                                                int64_t totalVolumeSteps,
                                                int64_t sumEntryPriceTicksVolume,
                                                Price markPrice,
                                                const SymbolSpec& spec);

// Basket-level P/L breakdown. Net = Gross − Commission − Spread − Swap −
// Slippage, exactly as specified in proposal §6.
struct BasketPnlInput {
    double grossPnlMoney = 0.0;
    double totalVolumeLots = 0.0; // sum of traded lots, for commission calc
    double spreadCostMoney = 0.0;
    double swapMoney = 0.0;
    double slippageMoney = 0.0; // pass actual when known, else an estimate
};

struct BasketPnlResult {
    double grossPnlMoney = 0.0;
    double commissionMoney = 0.0;
    double spreadCostMoney = 0.0;
    double swapMoney = 0.0;
    double slippageMoney = 0.0;
    double netPnlMoney = 0.0;
};

[[nodiscard]] BasketPnlResult computeBasketPnl(const BasketPnlInput& input,
                                                const CostInputs& costs);

}  // namespace muuk::core
