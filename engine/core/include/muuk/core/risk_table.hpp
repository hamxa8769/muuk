#pragma once

#include <vector>

#include "muuk/core/lot_progression.hpp"
#include "muuk/core/pnl.hpp"
#include "muuk/core/types.hpp"

namespace muuk::core {

// Worst case of the range strategy, level by level: price bounces between
// the two boundaries, so every fill adds the next lot on the other side.
// Fill 1 is a Buy at the top, fill 2 a Sell at the bottom, and so on.
struct RiskTableInput {
    LotProgressionConfig lots;
    SymbolSpec spec;
    CostInputs costs;
    double rangeWidth = 2.0;  // price distance between the boundaries
    double price = 4000.0;    // for commission and margin
    double spread = 0.30;     // price units, paid in and out
    double leverage = 500.0;  // 1:leverage
    double maxLot = 100.0;    // stop at the first lot above this
    int levels = 12;
};

struct RiskRow {
    int level = 0;
    double lot = 0.0;          // this fill
    double totalLots = 0.0;    // all open
    double netLots = 0.0;      // + long, - short
    double worstLoss = 0.0;    // floating at this fill, bounce loss + commission + spread (positive = loss)
    double margin = 0.0;       // larger side x contract x price / leverage
    double recoveryMove = 0.0; // price move past the last fill that brings the basket back to 0 (0 = none possible)
};

[[nodiscard]] std::vector<RiskRow> riskTable(const RiskTableInput& in);

}  // namespace muuk::core
