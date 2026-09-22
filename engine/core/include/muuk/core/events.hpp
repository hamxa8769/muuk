#pragma once

#include <cstdint>
#include <variant>

#include "muuk/core/fixed_vector.hpp"
#include "muuk/core/orders.hpp"
#include "muuk/core/risk.hpp"
#include "muuk/core/types.hpp"

namespace muuk::core {

// --- Inputs (fed into StrategyEngine) --------------------------------------

struct TickEvent {
    Price bid;
    Price ask;
    int64_t timestampUnixMillis = 0;
};

enum class RejectReason {
    Unknown,
    InvalidPrice,
    InvalidVolume,
    SpreadTooWide,
    BrokerRejected,
};

struct OrderConfirmedEvent {
    OrderId id = kInvalidOrderId;
    Price fillPrice;
    int64_t timestampUnixMillis = 0;
};

struct OrderRejectedEvent {
    OrderId id = kInvalidOrderId;
    RejectReason reason = RejectReason::Unknown;
};

// --- Outputs (emitted by StrategyEngine, executed by the driver) -----------

struct PlaceOrderCommand {
    OrderId id = kInvalidOrderId;
    int sequenceIndex = 0;
    OrderSide side = OrderSide::BuyStop;
    Price price;
    Volume volume;
};

struct CancelOrderCommand {
    OrderId id = kInvalidOrderId;
};

struct CloseBasketCommand {
    double netPnlMoney = 0.0;
};

struct RiskAlertCommand {
    RiskLimitKind kind = RiskLimitKind::None;
};

using Command = std::variant<PlaceOrderCommand, CancelOrderCommand, CloseBasketCommand, RiskAlertCommand>;

// Upper bound on how many orders StrategyEngine will ever track as pending
// at once. The two-boundary grid strategy has a structural invariant of at
// most 2 (one per side, replaced — never accumulated — on fill/reject); this
// leaves headroom for that invariant to be wrong in a way tests didn't
// catch, and for future strategy variants with more simultaneous legs,
// without inflating CommandBatch (see kMaxCommandsPerEvent) to the point
// where returning one by value stops being cheap. Shared by StrategyEngine's
// internal storage and by kMaxCommandsPerEvent below so the two can never
// drift out of sync.
inline constexpr std::size_t kMaxTrackedOrders = 8;

// Upper bound on commands emitted in response to a single input event. The
// worst case is a basket close: one CloseBasketCommand plus a
// CancelOrderCommand for every still-pending order.
inline constexpr std::size_t kMaxCommandsPerEvent = kMaxTrackedOrders + 1;
using CommandBatch = FixedVector<Command, kMaxCommandsPerEvent>;

}  // namespace muuk::core
