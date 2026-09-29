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
    double balance = 0.0;          // >0: fills in RiskRow::marginLevel
    double hedgedPercent = 100.0;  // see MarginRule
};

struct RiskRow {
    int level = 0;
    double lot = 0.0;          // this fill
    double totalLots = 0.0;    // all open
    double netLots = 0.0;      // + long, - short
    double worstLoss = 0.0;    // floating at this fill, bounce loss + commission + spread (positive = loss)
    double margin = 0.0;       // see bookMargin
    double recoveryMove = 0.0; // price move past the last fill that brings the basket back to 0 (0 = none possible)
    double marginLevel = 0.0;  // % = (balance - worstLoss) / margin; 0 = no balance given
};

[[nodiscard]] std::vector<RiskRow> riskTable(const RiskTableInput& in);

// How a broker charges margin on a hedging account. The part of the smaller
// side that is matched by the larger one ("hedged" volume) is charged at
// hedgedPercent of a normal position, once; the rest at 100 %.
//   cTrader (IC Markets): the larger side only -> 100.
//   MT5: the symbol's SYMBOL_MARGIN_HEDGED / contract size, e.g. 50 or 0.
struct MarginRule {
    double leverage = 500.0;
    double hedgedPercent = 100.0;
};

// Margin held for buyLots / sellLots at `price`.
[[nodiscard]] double bookMargin(double buyLots, double sellLots, double price, double contractSize,
                                const MarginRule& rule);
// Equity / margin in %; a very large number when no margin is used.
[[nodiscard]] double marginLevelPercent(double equity, double margin);

// The opposite ("reverse") trade for a basket in loss: how many lots to open
// against the net position so that the basket is back at 0 after the price
// moves `move` in the new trade's direction -- or, with move <= 0, just the
// net lots (a lock: the loss stops moving).
//   q = |net| + (loss + cost(q)) / (move x contract)
// cost(q) = the trade's own commission (open + close) + the spread.
struct ReverseTradeInput {
    SymbolSpec spec;
    CostInputs costs;
    MarginRule margin;
    double buyLots = 0.0;
    double sellLots = 0.0;
    double floating = 0.0;  // the basket's net P/L now (negative = loss)
    double equity = 0.0;    // account equity now (balance + floating)
    double price = 4000.0;
    double spread = 0.30;
    double move = 0.0;      // <=0: lock only
    double maxLot = 100.0;
};

struct ReverseTrade {
    bool buy = false;          // side of the reverse trade
    double lot = 0.0;          // rounded up to the volume step
    double cost = 0.0;         // commission (open + close) + spread of this trade
    double marginAfter = 0.0;  // margin of the whole book once it's open
    double freeMarginAfter = 0.0;
    double marginLevelAfter = 0.0;  // %
    double lossAfter = 0.0;    // the frozen amount right after it opens (lock), or 0 target
    bool lockOnly = false;
    bool capped = false;       // needed more than maxLot
};

[[nodiscard]] ReverseTrade reverseTrade(const ReverseTradeInput& in);

}  // namespace muuk::core
