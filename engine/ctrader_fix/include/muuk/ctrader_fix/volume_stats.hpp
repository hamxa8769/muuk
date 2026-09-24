#pragma once

#include <string>
#include <vector>

#include "muuk/ctrader_fix/basket_history.hpp"

namespace muuk::ctrader_fix {

enum class VolumePeriod { Day, Week, Month };

// Trading volume and results for one period (or all time), from closed
// baskets. A basket counts in the period it closed in.
struct VolumeBucket {
    std::string period;        // "2026-09-24", "Week of 2026-09-21", "2026-09" -- or "All time"
    int baskets = 0;
    int won = 0;               // baskets closed with net > 0
    int lost = 0;              // baskets closed with net < 0
    int positions = 0;
    double lots = 0.0;         // lots traded, each position counted once (opened and closed = one round turn)
    double notional = 0.0;     // value traded when opening: lots x contract size x entry price (USD)
    double wonLots = 0.0;      // lots in baskets that closed in profit
    double lostLots = 0.0;     // lots in baskets that closed in loss
    double gross = 0.0;
    double commission = 0.0;
    double net = 0.0;
    // Fills vs the Stop prices the orders were placed at, in money: >0 = filled
    // worse. Already inside gross/net (those come from the real fills).
    double slippage = 0.0;

    // Brokers usually count volume per side: opening and closing each count.
    [[nodiscard]] double sideLots() const { return lots * 2.0; }
    [[nodiscard]] double netPerLot() const { return lots > 0.0 ? net / lots : 0.0; }
    void add(const BasketRecord& basket);
};

// One position's slippage in money (>0 = filled worse than its order
// price); 0 when the order price isn't known.
[[nodiscard]] double legSlippage(const BasketLegRecord& leg, double contractSize);

// The period a "YYYY-MM-DD HH:MM:SS" timestamp falls in. Weeks start on Monday.
[[nodiscard]] std::string periodKey(const std::string& timestamp, VolumePeriod period);

// One bucket per period that has baskets, newest first.
[[nodiscard]] std::vector<VolumeBucket> volumeByPeriod(const std::vector<BasketRecord>& baskets, VolumePeriod period);
[[nodiscard]] VolumeBucket volumeTotal(const std::vector<BasketRecord>& baskets);
// One bucket per trading session ("London + New York", "Tokyo"...) the
// baskets started in, most baskets first. Older records without the
// session are placed by their start (or close) time.
[[nodiscard]] std::vector<VolumeBucket> volumeBySession(const std::vector<BasketRecord>& baskets);

}  // namespace muuk::ctrader_fix
