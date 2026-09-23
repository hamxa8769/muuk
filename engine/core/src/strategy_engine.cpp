#include "muuk/core/strategy_engine.hpp"

#include <algorithm>
#include <cassert>

namespace muuk::core {

StrategyEngine::StrategyEngine(StrategyConfig config) : config_(config) {}

Price StrategyEngine::boundaryPriceForSide(OrderSide side) const {
    return side == OrderSide::BuyStop ? config_.range.upperBound : config_.range.lowerBound;
}

void StrategyEngine::resetCycleState() {
    pendingOrders_.clear();
    buyAggregate_ = SideAggregate{};
    sellAggregate_ = SideAggregate{};
    nextSequenceIndex_ = 1;
    slippageMoneyAccrued_ = 0.0;
    swapMoneyAccrued_ = 0.0;
    lastBasketPnl_ = BasketPnlResult{};
}

void StrategyEngine::tryArmOrder(CommandBatch& out, OrderSide side, int seqIndex, int retryCount) {
    const Volume lot = computeSequenceLot(config_.lotProgression, seqIndex, config_.symbol);

    RiskCheckInput riskInput;
    riskInput.activeOrderCount = static_cast<int>(pendingOrders_.size());
    riskInput.nextOrderVolume = lot;
    // Current floating loss doubles as a Phase-1 drawdown proxy. A true
    // peak-to-trough drawdown series is a backtest-level statistic (see
    // backtester::StatsCollector), not something the live decision path
    // needs to hold onto.
    riskInput.basketLossMoney = std::max(0.0, -lastBasketPnl_.netPnlMoney);
    riskInput.drawdownMoney = riskInput.basketLossMoney;
    // Margin usage requires account equity/leverage data that this engine
    // does not own (it belongs to a broker/account adapter added in a later
    // phase). Left at 0 == "unconstrained" until that adapter exists.
    riskInput.marginUsageFraction = 0.0;
    riskInput.currentSpreadTicks = lastTick_.ask.ticks - lastTick_.bid.ticks;
    riskInput.estimatedSlippageTicks = static_cast<int64_t>(config_.costs.slippageBufferTicks);

    const RiskLimitKind breach = evaluateRisk(config_.riskLimits, riskInput);
    if (breach != RiskLimitKind::None) {
        out.push_back(RiskAlertCommand{breach});
        status_ = EngineStatus::HaltedByRisk;
        return;
    }

    const Price price = boundaryPriceForSide(side);
    PendingOrder order{allocateOrderId(), seqIndex, side, price, lot, OrderState::Pending};
    pendingOrders_.push_back(ArmedOrder{order, retryCount});
    out.push_back(PlaceOrderCommand{order.id, seqIndex, side, price, lot});
}

CommandBatch StrategyEngine::armInitialOrders() {
    resetCycleState();
    CommandBatch out;

    tryArmOrder(out, OrderSide::BuyStop, nextSequenceIndex_);
    ++nextSequenceIndex_;

    if (status_ != EngineStatus::HaltedByRisk) {
        tryArmOrder(out, OrderSide::SellStop, nextSequenceIndex_);
        ++nextSequenceIndex_;
    }

    if (status_ != EngineStatus::HaltedByRisk) {
        status_ = EngineStatus::Active;
    }
    return out;
}

CommandBatch StrategyEngine::start() {
    assert(status_ == EngineStatus::Idle && "start() must be called exactly once from Idle");
    return armInitialOrders();
}

CommandBatch StrategyEngine::restart() {
    assert(status_ == EngineStatus::Completed && "restart() is only valid after a basket close");
    return armInitialOrders();
}

BasketPnlResult StrategyEngine::evaluateBasketPnl(const TickEvent& tick) const {
    // Gross P/L is computed at the theoretical mid price; the cost of
    // actually crossing the spread to exit is charged explicitly below as
    // spreadCostMoney, so it is not double-counted by also marking legs at
    // bid/ask.
    const Price mid{(tick.bid.ticks + tick.ask.ticks) / 2};

    const double grossPnl =
        aggregatePositionPnlMoney(OrderSide::BuyStop, buyAggregate_.totalVolumeSteps,
                                   buyAggregate_.sumEntryPriceTicksVolume, mid, config_.symbol) +
        aggregatePositionPnlMoney(OrderSide::SellStop, sellAggregate_.totalVolumeSteps,
                                   sellAggregate_.sumEntryPriceTicksVolume, mid, config_.symbol);
    const int64_t totalVolumeSteps = buyAggregate_.totalVolumeSteps + sellAggregate_.totalVolumeSteps;
    const double totalLots = static_cast<double>(totalVolumeSteps) * config_.symbol.volumeStep;

    const int64_t halfSpreadTicks = std::max<int64_t>(0, (tick.ask.ticks - tick.bid.ticks) / 2);
    const double spreadCostMoney =
        static_cast<double>(halfSpreadTicks) * config_.symbol.tickValue * totalLots;

    BasketPnlInput input;
    input.grossPnlMoney = grossPnl;
    input.totalVolumeLots = totalLots;
    input.spreadCostMoney = spreadCostMoney;
    input.swapMoney = swapMoneyAccrued_;
    input.slippageMoney = slippageMoneyAccrued_;

    return computeBasketPnl(input, config_.costs);
}

CommandBatch StrategyEngine::onTick(const TickEvent& tick) {
    lastTick_ = tick;
    CommandBatch out;

    if (status_ != EngineStatus::Active) {
        return out;
    }

    lastBasketPnl_ = evaluateBasketPnl(tick);

    const bool haveOpenExposure = (buyAggregate_.totalVolumeSteps + sellAggregate_.totalVolumeSteps) > 0;
    if (haveOpenExposure && config_.basketProfitTargetMoney > 0.0 &&
        lastBasketPnl_.netPnlMoney >= config_.basketProfitTargetMoney) {
        out.push_back(CloseBasketCommand{lastBasketPnl_.netPnlMoney});
        for (const auto& armed : pendingOrders_) {
            out.push_back(CancelOrderCommand{armed.order.id});
        }
        pendingOrders_.clear();
        status_ = EngineStatus::Completed;
    }

    return out;
}

CommandBatch StrategyEngine::onOrderConfirmed(const OrderConfirmedEvent& event) {
    CommandBatch out;

    std::size_t foundIndex = pendingOrders_.size();
    for (std::size_t i = 0; i < pendingOrders_.size(); ++i) {
        if (pendingOrders_[i].order.id == event.id) {
            foundIndex = i;
            break;
        }
    }
    // Unknown or already-processed id: treat as a no-op rather than acting
    // twice on the same fill (proposal §4: avoid duplicate orders).
    if (foundIndex == pendingOrders_.size()) {
        return out;
    }

    const ArmedOrder armed = pendingOrders_[foundIndex];
    pendingOrders_.erase_at(foundIndex);

    const int64_t adverseTicks =
        (armed.order.side == OrderSide::BuyStop)
            ? std::max<int64_t>(0, event.fillPrice.ticks - armed.order.price.ticks)
            : std::max<int64_t>(0, armed.order.price.ticks - event.fillPrice.ticks);
    slippageMoneyAccrued_ += static_cast<double>(adverseTicks) * config_.symbol.tickValue *
                              armed.order.volume.toDouble(config_.symbol);

    SideAggregate& agg = (armed.order.side == OrderSide::BuyStop) ? buyAggregate_ : sellAggregate_;
    agg.totalVolumeSteps += armed.order.volume.steps;
    agg.sumEntryPriceTicksVolume += event.fillPrice.ticks * armed.order.volume.steps;

    if (status_ == EngineStatus::HaltedByRisk || !config_.reArmOnFill) {
        return out;
    }

    const int seq = nextSequenceIndex_++;
    tryArmOrder(out, armed.order.side, seq);
    return out;
}

CommandBatch StrategyEngine::onOrderRejected(const OrderRejectedEvent& event) {
    CommandBatch out;

    std::size_t foundIndex = pendingOrders_.size();
    for (std::size_t i = 0; i < pendingOrders_.size(); ++i) {
        if (pendingOrders_[i].order.id == event.id) {
            foundIndex = i;
            break;
        }
    }
    if (foundIndex == pendingOrders_.size()) {
        return out;
    }

    const ArmedOrder armed = pendingOrders_[foundIndex];
    pendingOrders_.erase_at(foundIndex);

    if (status_ == EngineStatus::HaltedByRisk) {
        return out;
    }

    if (armed.retryCount < config_.maxRejectRetries) {
        tryArmOrder(out, armed.order.side, armed.order.sequenceIndex, armed.retryCount + 1);
    }
    // Retries exhausted: give up silently on this side until the next
    // restart(). No further orders are armed for it in this cycle.

    return out;
}

}  // namespace muuk::core
