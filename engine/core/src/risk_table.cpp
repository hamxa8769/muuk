#include "muuk/core/risk_table.hpp"

#include <algorithm>
#include <cmath>

namespace muuk::core {

std::vector<RiskRow> riskTable(const RiskTableInput& in) {
    std::vector<RiskRow> rows;
    const double contract = in.spec.contractSize;
    const double leverage = std::max(1.0, in.leverage);
    double buys = 0.0;
    double sells = 0.0;
    double commission = 0.0;
    for (int n = 1; n <= in.levels; ++n) {
        const double lot = computeSequenceLot(in.lots, n, in.spec).toDouble(in.spec);
        if (in.maxLot > 0.0 && lot > in.maxLot + 1e-9) break;  // 0 = no max lot
        const bool buy = n % 2 == 1;
        (buy ? buys : sells) += lot;
        commission += roundTurnCommissionMoney(lot, in.price, in.spec, in.costs);
        const double total = buys + sells;
        // At a Buy fill price is at the top: every sell (opened at the bottom)
        // is a whole range width under water, and the other way round.
        const double otherSide = buy ? sells : buys;
        RiskRow r;
        r.level = n;
        r.lot = lot;
        r.totalLots = total;
        r.netLots = buys - sells;
        r.worstLoss = in.rangeWidth * contract * otherSide + commission + in.spread * contract * total;
        r.margin = bookMargin(buys, sells, in.price, contract, MarginRule{leverage, in.hedgedPercent});
        r.recoveryMove = std::fabs(r.netLots) > 1e-9 ? r.worstLoss / (std::fabs(r.netLots) * contract) : 0.0;
        if (in.balance > 0.0) r.marginLevel = marginLevelPercent(in.balance - r.worstLoss, r.margin);
        rows.push_back(r);
    }
    return rows;
}

double bookMargin(double buyLots, double sellLots, double price, double contractSize, const MarginRule& rule) {
    const double hedged = std::min(buyLots, sellLots);
    const double open = std::fabs(buyLots - sellLots);
    const double lots = open + hedged * std::clamp(rule.hedgedPercent, 0.0, 100.0) / 100.0;
    return lots * contractSize * price / std::max(1.0, rule.leverage);
}

double marginLevelPercent(double equity, double margin) {
    if (margin <= 1e-9) return 1e9;
    return equity / margin * 100.0;
}

ReverseTrade reverseTrade(const ReverseTradeInput& in) {
    ReverseTrade r;
    const double contract = in.spec.contractSize;
    const double net = in.buyLots - in.sellLots;
    const double step = in.spec.volumeStep > 0.0 ? in.spec.volumeStep : 0.01;
    // Long -> sell against it; flat -> the side doesn't matter for the size.
    r.buy = net < 0.0;
    const double loss = std::max(0.0, -in.floating);
    auto costOf = [&](double q) {
        return roundTurnCommissionMoney(q, in.price, in.spec, in.costs) + std::max(0.0, in.spread) * contract * q;
    };
    auto roundUp = [&](double q) { return std::ceil(q / step - 1e-9) * step; };
    double q = std::fabs(net);
    r.lockOnly = in.move <= 0.0;
    if (!r.lockOnly) {
        for (int i = 0; i < 30; ++i) {
            const double next = std::fabs(net) + (loss + costOf(q)) / (in.move * contract);
            if (std::fabs(next - q) < 1e-9) break;
            q = next;
        }
    }
    q = roundUp(q);
    if (in.maxLot > 0.0 && q > in.maxLot + 1e-9) {
        q = in.maxLot;
        r.capped = true;
    }
    r.lot = q;
    r.cost = costOf(q);
    const double buys = in.buyLots + (r.buy ? q : 0.0);
    const double sells = in.sellLots + (r.buy ? 0.0 : q);
    r.marginAfter = bookMargin(buys, sells, in.price, contract, in.margin);
    // Opening pays the spread at once; the commission is counted in full
    // (it's part of what closing the book costs).
    const double equityAfter = in.equity - r.cost;
    r.freeMarginAfter = equityAfter - r.marginAfter;
    r.marginLevelAfter = marginLevelPercent(equityAfter, r.marginAfter);
    r.lossAfter = r.lockOnly ? in.floating - r.cost : 0.0;
    return r;
}

}  // namespace muuk::core
