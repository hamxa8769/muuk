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
    seeding_ = false;
}

void StrategyEngine::tryArmOrder(CommandBatch& out, OrderSide side, int seqIndex, int retryCount,
                                  std::optional<Price> priceOverride) {
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

    const Price price = priceOverride.value_or(boundaryPriceForSide(side));
    PendingOrder order{allocateOrderId(), seqIndex, side, price, lot, OrderState::Pending};
    pendingOrders_.push_back(ArmedOrder{order, retryCount});
    out.push_back(PlaceOrderCommand{order.id, seqIndex, side, price, lot});
}

CommandBatch StrategyEngine::armInitialOrders() {
    resetCycleState();
    CommandBatch out;

    if (config_.seedOffsetPoints > 0.0) {
        // See StrategyConfig::seedOffsetPoints: arm two tiny seed orders
        // around a reference midpoint instead of the full boundaries —
        // the first one to fill anchors the real range in
        // onOrderConfirmed. The reference is the last tick this engine
        // has actually seen (lastTick_ persists across resetCycleState(),
        // so on restart() after a basket close it's the very tick that
        // triggered the close — always fresh) when one exists; before the
        // very first tick ever (the first start() call), there is none
        // yet, so config_.range's midpoint — set by the caller from
        // whatever it knew at construction time (e.g. a live driver's own
        // most recent quote) — is used instead.
        seeding_ = true;
        const bool haveTick = lastTick_.bid.ticks != 0 || lastTick_.ask.ticks != 0;
        const Price mid = haveTick ? Price{(lastTick_.bid.ticks + lastTick_.ask.ticks) / 2}
                                    : Price{(config_.range.lowerBound.ticks + config_.range.upperBound.ticks) / 2};
        const int64_t offsetTicks =
            std::max<int64_t>(1, Price::fromDouble(config_.seedOffsetPoints, config_.symbol).ticks);
        const Price seedUpper{mid.ticks + offsetTicks};
        const Price seedLower{mid.ticks - offsetTicks};

        tryArmOrder(out, OrderSide::BuyStop, nextSequenceIndex_, 0, seedUpper);
        ++nextSequenceIndex_;
        if (status_ != EngineStatus::HaltedByRisk) {
            tryArmOrder(out, OrderSide::SellStop, nextSequenceIndex_, 0, seedLower);
            ++nextSequenceIndex_;
        }
    } else {
        tryArmOrder(out, OrderSide::BuyStop, nextSequenceIndex_);
        ++nextSequenceIndex_;

        if (status_ != EngineStatus::HaltedByRisk) {
            tryArmOrder(out, OrderSide::SellStop, nextSequenceIndex_);
            ++nextSequenceIndex_;
        }
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

    if (status_ == EngineStatus::HaltedByRisk) {
        return out;
    }

    if (seeding_) {
        // This is the first-ever fill of the cycle in seed-anchor mode:
        // whichever seed just filled wins. Cancel the other one (if risk
        // let it get armed and it's still pending) and re-derive the real
        // range from this confirmed fill, keeping the originally
        // configured total width. Falls through to the normal re-arm
        // logic below — no special-casing of *which* side gets armed
        // next; that's the same rule either way.
        seeding_ = false;
        for (const auto& other : pendingOrders_) {
            out.push_back(CancelOrderCommand{other.order.id});
        }
        pendingOrders_.clear();

        const int64_t width = config_.range.upperBound.ticks - config_.range.lowerBound.ticks;
        if (armed.order.side == OrderSide::BuyStop) {
            config_.range.upperBound = event.fillPrice;
            config_.range.lowerBound = Price{event.fillPrice.ticks - width};
        } else {
            config_.range.lowerBound = event.fillPrice;
            config_.range.upperBound = Price{event.fillPrice.ticks + width};
        }
    }

    if (!config_.reArmOnFill) {
        return out;
    }

    // Proposal §3/§4: a fill arms the *opposite* side's next order (never
    // the side that just filled again) — e.g. Buy Stop fills, Sell Stop
    // (already pending from the initial pair) is left alone; only once
    // *that* fills does a fresh Buy Stop get armed, and so on,
    // alternating one pending order at a time. Re-arming the same side
    // instead (an earlier version of this code did) lets one side
    // accumulate unbounded fills whenever price keeps moving the same
    // direction, since it never needs the other side to fill first.
    const OrderSide opposite = (armed.order.side == OrderSide::BuyStop) ? OrderSide::SellStop : OrderSide::BuyStop;
    bool oppositeAlreadyPending = false;
    for (const auto& p : pendingOrders_) {
        if (p.order.side == opposite) {
            oppositeAlreadyPending = true;
            break;
        }
    }
    if (oppositeAlreadyPending) {
        return out;
    }

    const int seq = nextSequenceIndex_++;
    tryArmOrder(out, opposite, seq);
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
