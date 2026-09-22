#pragma once

#include <vector>

#include "muuk/backtester/stats_collector.hpp"
#include "muuk/backtester/tick_data_source.hpp"
#include "muuk/core/strategy_engine.hpp"

namespace muuk::backtester {

// Deterministic fill assumptions used when no real broker is present. A
// pending order triggers the instant the market touches its price (Buy
// Stop when ask >= price, Sell Stop when bid <= price); an optional fixed
// slippage buffer is applied in the adverse direction to keep results
// conservative. Proposal §7: "a conservative configurable slippage buffer
// can be used" for pre-trade decisions when actual slippage isn't known.
struct FillSimulatorConfig {
    double slippageBufferTicks = 0.0;
};

struct BacktestConfig {
    muuk::core::StrategyConfig strategy;
    FillSimulatorConfig fillSim;
};

// Replays historical ticks through a StrategyEngine exactly as a live
// driver would: it detects pending-order triggers, feeds confirmations
// back into the engine, executes the commands the engine emits (arming
// the mirrored order book, closing baskets, restarting new cycles), and
// aggregates results via StatsCollector. Proposal §20 (Phase 2) /
// §21: the backtester and (future) live driver share the exact same
// StrategyEngine — this class contains no strategy decision logic itself.
class BacktestRunner {
public:
    explicit BacktestRunner(BacktestConfig config);

    [[nodiscard]] BacktestReport run(const std::vector<TickRecord>& ticks);

private:
    void handleCommand(const muuk::core::Command& cmd);
    void checkTriggers(const TickRecord& tick);

    BacktestConfig config_;
    muuk::core::StrategyEngine engine_;
    std::vector<muuk::core::PendingOrder> book_; // mirrors the engine's currently-armed orders
    StatsCollector stats_;
};

}  // namespace muuk::backtester
