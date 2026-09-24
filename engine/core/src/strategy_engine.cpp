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
        const Price rangeMid{(config_.range.lowerBound.ticks + config_.range.upperBound.ticks) / 2};
        // Offsets are measured from the side of the book each Stop order
        // actually triggers on (Buy Stop: ask, Sell Stop: bid), not from
        // the mid. Measured from the mid, an offset smaller than half the
        // spread puts the Buy Stop below the ask and the Sell Stop above
        // the bid -- both trigger instantly, opening two positions at once.
        const Price refAsk = haveTick ? lastTick_.ask : rangeMid;
        const Price refBid = haveTick ? lastTick_.bid : rangeMid;
        const int64_t offsetTicks =
            std::max<int64_t>(1, Price::fromDouble(config_.seedOffsetPoints, config_.symbol).ticks);
        const Price seedUpper{refAsk.ticks + offsetTicks};
        const Price seedLower{refBid.ticks - offsetTicks};

        // Both seeds share one sequence slot: exactly one of them ever
        // actually becomes a trade (the other is always cancelled
        // unfilled in onOrderConfirmed's seeding branch), so only one
        // slot in the lot progression should ever be spent on this pair.
        // Advancing nextSequenceIndex_ twice here (once per seed) used to
        // make the *next* real order jump straight from L(1) to L(3) —
        // e.g. 0.01 then 0.03, silently skipping 0.02 — because the
        // cancelled seed's slot was never actually used by a fill.
        const int seedSeq = nextSequenceIndex_;
        if (config_.startSide != StartSide::SellOnly) {
            tryArmOrder(out, OrderSide::BuyStop, seedSeq, 0, seedUpper);
        }
        if (config_.startSide != StartSide::BuyOnly && status_ != EngineStatus::HaltedByRisk) {
            tryArmOrder(out, OrderSide::SellStop, seedSeq, 0, seedLower);
        }
        ++nextSequenceIndex_;
    } else {
        if (config_.startSide != StartSide::SellOnly) {
            tryArmOrder(out, OrderSide::BuyStop, nextSequenceIndex_);
            ++nextSequenceIndex_;
        }
        if (config_.startSide != StartSide::BuyOnly && status_ != EngineStatus::HaltedByRisk) {
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

void StrategyEngine::stopCycle() {
    resetCycleState();
    status_ = EngineStatus::Idle;
}

void StrategyEngine::reconfigure(const StrategyConfig& config) {
    assert((status_ == EngineStatus::Idle || status_ == EngineStatus::Completed) &&
           "reconfigure() is only valid while no cycle is running");
    config_ = config;
}

BasketPnlResult StrategyEngine::evaluateBasketPnl(const TickEvent& tick) const {
    // Gross P/L is computed at the theoretical mid price; the cost of
    // actually crossing the spread to exit is charged explicitly below as
    // spreadCostMoney, so it is not double-counted by also marking legs at
    // bid/ask. Both use the exact (possibly half-tick) mid: rounding it to
    // a whole tick made an odd spread (e.g. 5 ticks) cost only 2 ticks per
    // side instead of 2.5, so the target could trigger a little early and
    // disagree with the per-position figures a UI shows.
    const double midTicks = (static_cast<double>(tick.bid.ticks) + static_cast<double>(tick.ask.ticks)) / 2.0;
    const double moneyPerTickStep = config_.symbol.tickValue * config_.symbol.volumeStep;

    const double grossPnl =
        (midTicks * static_cast<double>(buyAggregate_.totalVolumeSteps) -
         static_cast<double>(buyAggregate_.sumEntryPriceTicksVolume) +
         static_cast<double>(sellAggregate_.sumEntryPriceTicksVolume) -
         midTicks * static_cast<double>(sellAggregate_.totalVolumeSteps)) *
        moneyPerTickStep;
    const int64_t totalVolumeSteps = buyAggregate_.totalVolumeSteps + sellAggregate_.totalVolumeSteps;
    const double totalLots = static_cast<double>(totalVolumeSteps) * config_.symbol.volumeStep;

    // Closing costs half the spread per lot either way (a buy exits at the
    // bid, a sell at the ask, both measured from the mid).
    const double halfSpreadTicks =
        std::max(0.0, static_cast<double>(tick.ask.ticks - tick.bid.ticks) / 2.0);
    const double spreadCostMoney = halfSpreadTicks * config_.symbol.tickValue * totalLots;

    BasketPnlInput input;
    input.grossPnlMoney = grossPnl;
    input.totalVolumeLots = totalLots;
    input.spreadCostMoney = spreadCostMoney;
    input.swapMoney = swapMoneyAccrued_;
    input.slippageMoney = slippageMoneyAccrued_;

    return computeBasketPnl(input, config_.costs);
}

EngineState StrategyEngine::exportState() const {
    EngineState s;
    s.status = status_;
    s.range = config_.range;
    for (const auto& armed : pendingOrders_) s.pendingOrders.push_back(EngineState::Armed{armed.order, armed.retryCount});
    s.buyVolumeSteps = buyAggregate_.totalVolumeSteps;
    s.buySumEntryPriceTicksVolume = buyAggregate_.sumEntryPriceTicksVolume;
    s.sellVolumeSteps = sellAggregate_.totalVolumeSteps;
    s.sellSumEntryPriceTicksVolume = sellAggregate_.sumEntryPriceTicksVolume;
    s.nextSequenceIndex = nextSequenceIndex_;
    s.nextOrderId = nextOrderId_;
    s.seeding = seeding_;
    s.slippageMoneyAccrued = slippageMoneyAccrued_;
    s.swapMoneyAccrued = swapMoneyAccrued_;
    return s;
}

void StrategyEngine::restoreState(const EngineState& s) {
    resetCycleState();
    status_ = s.status;
    // An idle state has no range of its own -- the next start() uses the
    // configured one, so keep it.
    if (s.status != EngineStatus::Idle) config_.range = s.range;
    for (const auto& armed : s.pendingOrders) pendingOrders_.push_back(ArmedOrder{armed.order, armed.retryCount});
    buyAggregate_ = SideAggregate{s.buyVolumeSteps, s.buySumEntryPriceTicksVolume};
    sellAggregate_ = SideAggregate{s.sellVolumeSteps, s.sellSumEntryPriceTicksVolume};
    nextSequenceIndex_ = s.nextSequenceIndex;
    nextOrderId_ = std::max<OrderId>(nextOrderId_, s.nextOrderId);
    seeding_ = s.seeding;
    slippageMoneyAccrued_ = s.slippageMoneyAccrued;
    swapMoneyAccrued_ = s.swapMoneyAccrued;
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
        const int64_t netSteps = buyAggregate_.totalVolumeSteps - sellAggregate_.totalVolumeSteps;
        const OrderSide closeSide = netSteps >= 0 ? OrderSide::BuyStop : OrderSide::SellStop;
        const Volume closeVolume{netSteps >= 0 ? netSteps : -netSteps};
        out.push_back(CloseBasketCommand{lastBasketPnl_.netPnlMoney, closeSide, closeVolume});
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

    // Gross P/L is measured from the price each order was *meant* to fill
    // at, and the gap to the actual fill is charged separately as slippage
    // (signed: a better-than-requested fill reduces it). That keeps
    // Net = Gross - Slippage - Spread - Commission - Swap exact. Measuring
    // gross from the actual fill *and* charging slippage (as an earlier
    // version did) counted every slipped tick twice.
    const int64_t slippageTicks = (armed.order.side == OrderSide::BuyStop)
                                      ? event.fillPrice.ticks - armed.order.price.ticks
                                      : armed.order.price.ticks - event.fillPrice.ticks;
    slippageMoneyAccrued_ += static_cast<double>(slippageTicks) * config_.symbol.tickValue *
                              armed.order.volume.toDouble(config_.symbol);

    SideAggregate& agg = (armed.order.side == OrderSide::BuyStop) ? buyAggregate_ : sellAggregate_;
    agg.totalVolumeSteps += armed.order.volume.steps;
    agg.sumEntryPriceTicksVolume += armed.order.price.ticks * armed.order.volume.steps;

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
