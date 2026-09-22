#pragma once

#include <cstdint>

#include "muuk/core/types.hpp"

namespace muuk::core {

using OrderId = uint64_t;
inline constexpr OrderId kInvalidOrderId = 0;

enum class OrderSide { BuyStop, SellStop };

// A fixed price range with pending orders at the two boundaries. Proposal §3:
// lowerBound = Sell Stop price, upperBound = Buy Stop price.
struct RangeConfig {
    Price lowerBound;
    Price upperBound;
};

enum class OrderState { Pending, Filled, Cancelled, Rejected };

// A single order in the progressive sequence, from the engine's point of
// view (not the broker's own order representation).
struct PendingOrder {
    OrderId id = kInvalidOrderId;
    int sequenceIndex = 0; // 1-based position in the lot progression
    OrderSide side = OrderSide::BuyStop;
    Price price;
    Volume volume;
    OrderState state = OrderState::Pending;
};

}  // namespace muuk::core
