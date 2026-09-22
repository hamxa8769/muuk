#include "muuk/backtester/backtest_runner.hpp"

#include <algorithm>

using namespace muuk::core;

namespace muuk::backtester {

BacktestRunner::BacktestRunner(BacktestConfig config)
    : config_(config), engine_(config_.strategy) {}

void BacktestRunner::handleCommand(const Command& cmd) {
    if (const auto* place = std::get_if<PlaceOrderCommand>(&cmd)) {
        book_.push_back(PendingOrder{place->id, place->sequenceIndex, place->side, place->price,
                                      place->volume, OrderState::Pending});
        stats_.onOrderPlaced();
    } else if (const auto* cancel = std::get_if<CancelOrderCommand>(&cmd)) {
        std::erase_if(book_, [&](const PendingOrder& o) { return o.id == cancel->id; });
    } else if (const auto* close = std::get_if<CloseBasketCommand>(&cmd)) {
        stats_.onBasketClosed(close->netPnlMoney);
    } else if (const auto* alert = std::get_if<RiskAlertCommand>(&cmd)) {
        stats_.onRiskAlert(alert->kind);
    }
}

void BacktestRunner::checkTriggers(const TickRecord& tick) {
    const SymbolSpec& spec = config_.strategy.symbol;
    const Price bid = Price::fromDouble(tick.bid, spec);
    const Price ask = Price::fromDouble(tick.ask, spec);

    // Snapshot which orders trigger on this tick before mutating book_, so
    // a re-armed order placed as a reaction to one fill is only considered
    // starting on the *next* tick (it did not exist yet when this tick's
    // price was observed).
    std::vector<OrderId> triggered;
    for (const auto& order : book_) {
        const bool hit = (order.side == OrderSide::BuyStop) ? (ask.ticks >= order.price.ticks)
                                                              : (bid.ticks <= order.price.ticks);
        if (hit) triggered.push_back(order.id);
    }

    for (const OrderId id : triggered) {
        const auto it = std::find_if(book_.begin(), book_.end(),
                                      [&](const PendingOrder& o) { return o.id == id; });
        if (it == book_.end()) continue; // already removed by an earlier command this tick

        const PendingOrder order = *it;
        book_.erase(it);

        const int64_t slippageTicks = static_cast<int64_t>(config_.fillSim.slippageBufferTicks);
        Price fillPrice = order.price;
        fillPrice.ticks += (order.side == OrderSide::BuyStop) ? slippageTicks : -slippageTicks;

        stats_.onOrderFilled();
        const CommandBatch out =
            engine_.onOrderConfirmed(OrderConfirmedEvent{id, fillPrice, tick.timestampUnixMillis});
        for (const auto& cmd : out) handleCommand(cmd);
    }
}

BacktestReport BacktestRunner::run(const std::vector<TickRecord>& ticks) {
    for (const auto& cmd : engine_.start()) handleCommand(cmd);

    for (const auto& tick : ticks) {
        checkTriggers(tick);

        const SymbolSpec& spec = config_.strategy.symbol;
        const TickEvent event{Price::fromDouble(tick.bid, spec), Price::fromDouble(tick.ask, spec),
                               tick.timestampUnixMillis};
        for (const auto& cmd : engine_.onTick(event)) handleCommand(cmd);
        stats_.onTickProcessed();

        if (engine_.status() == EngineStatus::Completed) {
            for (const auto& cmd : engine_.restart()) handleCommand(cmd);
        }
        if (engine_.status() == EngineStatus::HaltedByRisk) {
            break; // proposal §8: a hard risk limit stops new order creation
        }
    }

    return stats_.buildReport();
}

}  // namespace muuk::backtester
