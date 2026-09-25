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
        if (lot > in.maxLot + 1e-9) break;
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
        r.margin = std::max(buys, sells) * contract * in.price / leverage;
        r.recoveryMove = std::fabs(r.netLots) > 1e-9 ? r.worstLoss / (std::fabs(r.netLots) * contract) : 0.0;
        rows.push_back(r);
    }
    return rows;
}

}  // namespace muuk::core
