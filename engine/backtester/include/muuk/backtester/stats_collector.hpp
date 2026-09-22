#pragma once

#include "muuk/core/risk.hpp"

namespace muuk::backtester {

// Summary statistics for a full backtest run. Proposal §20 (Phase 2):
// "historical testing, drawdown, worst sequences, net P/L and risk
// statistics."
struct BacktestReport {
    double netPnlMoney = 0.0;               // sum of all closed-basket net P/L
    double maxDrawdownMoney = 0.0;          // largest peak-to-trough drop in cumulative realized P/L
    double worstBasketLossMoney = 0.0;      // most negative single-basket close (0 if none were negative)
    int basketsClosed = 0;
    int riskHalts = 0;
    int ordersPlaced = 0;
    int ordersFilled = 0;
    int ticksProcessed = 0;
};

// Accumulates events emitted while replaying a backtest and produces a
// BacktestReport. Kept separate from BacktestRunner so the aggregation
// logic (drawdown/worst-sequence tracking) is independently testable.
class StatsCollector {
public:
    void onTickProcessed();
    void onOrderPlaced();
    void onOrderFilled();
    void onBasketClosed(double netPnlMoney);
    void onRiskAlert(muuk::core::RiskLimitKind kind);

    [[nodiscard]] BacktestReport buildReport() const;

private:
    BacktestReport report_;
    double cumulativeRealizedPnlMoney_ = 0.0;
    double peakRealizedPnlMoney_ = 0.0;
};

}  // namespace muuk::backtester
