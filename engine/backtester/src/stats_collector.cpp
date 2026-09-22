#include "muuk/backtester/stats_collector.hpp"

#include <algorithm>

namespace muuk::backtester {

void StatsCollector::onTickProcessed() { ++report_.ticksProcessed; }

void StatsCollector::onOrderPlaced() { ++report_.ordersPlaced; }

void StatsCollector::onOrderFilled() { ++report_.ordersFilled; }

void StatsCollector::onBasketClosed(double netPnlMoney) {
    ++report_.basketsClosed;
    report_.worstBasketLossMoney = std::min(report_.worstBasketLossMoney, netPnlMoney);

    cumulativeRealizedPnlMoney_ += netPnlMoney;
    peakRealizedPnlMoney_ = std::max(peakRealizedPnlMoney_, cumulativeRealizedPnlMoney_);
    const double drawdown = peakRealizedPnlMoney_ - cumulativeRealizedPnlMoney_;
    report_.maxDrawdownMoney = std::max(report_.maxDrawdownMoney, drawdown);
}

void StatsCollector::onRiskAlert(muuk::core::RiskLimitKind /*kind*/) { ++report_.riskHalts; }

BacktestReport StatsCollector::buildReport() const {
    BacktestReport final = report_;
    final.netPnlMoney = cumulativeRealizedPnlMoney_;
    return final;
}

}  // namespace muuk::backtester
