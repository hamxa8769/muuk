#include "muuk/core/market_read.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace muuk::core {

void BarSeries::addQuote(int64_t utcMs, double mid) {
    if (!(mid > 0.0)) return;
    const int64_t start = utcMs - (((utcMs % barMs_) + barMs_) % barMs_);
    if (bars_.empty() || !lastIsForming_ || start > bars_.back().startMs) {
        bars_.push_back(Bar{start, mid, mid, mid, mid});
        lastIsForming_ = true;
        while (bars_.size() > keep_) bars_.pop_front();
        return;
    }
    Bar& b = bars_.back();  // this bar (or a clock that stepped back: keep filling the newest)
    b.high = std::max(b.high, mid);
    b.low = std::min(b.low, mid);
    b.close = mid;
}

std::deque<Bar> BarSeries::closed() const {
    std::deque<Bar> out(bars_);
    if (lastIsForming_ && !out.empty()) out.pop_back();
    return out;
}

std::size_t BarSeries::closedCount() const {
    return bars_.empty() ? 0 : bars_.size() - (lastIsForming_ ? 1 : 0);
}

void BarSeries::addClosed(const Bar& bar) {
    // Before any bar it already has (history arrives oldest first, ahead of live quotes).
    auto at = std::upper_bound(bars_.begin(), bars_.end(), bar.startMs,
                               [](int64_t ms, const Bar& b) { return ms < b.startMs; });
    if (at != bars_.begin() && std::prev(at)->startMs == bar.startMs) return;  // already there
    if (at == bars_.end() && lastIsForming_ && !bars_.empty()) at = std::prev(bars_.end());
    bars_.insert(at, bar);
    if (bars_.size() == 1) lastIsForming_ = false;
    while (bars_.size() > keep_) bars_.pop_front();
}

namespace {

std::string fmt(const char* f, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

}  // namespace

MarketRead readMarket(const std::deque<Bar>& bars, int lookback) {
    MarketRead r;
    lookback = std::max(4, lookback);
    const int need = std::max(lookback, 22);
    r.bars = static_cast<int>(bars.size());
    if (r.bars < need) {
        r.why = "collecting prices: " + std::to_string(r.bars) + " of " + std::to_string(need) + " bars";
        return r;
    }
    r.enoughData = true;

    // Trend: EMA 8 vs EMA 21 of the closes, and the slow one's slope.
    const double aFast = 2.0 / 9.0;
    const double aSlow = 2.0 / 22.0;
    double fast = bars.front().close;
    double slow = bars.front().close;
    std::vector<double> slowHistory;
    slowHistory.reserve(bars.size());
    for (const Bar& b : bars) {
        fast += aFast * (b.close - fast);
        slow += aSlow * (b.close - slow);
        slowHistory.push_back(slow);
    }
    r.emaFast = fast;
    r.emaSlow = slow;
    r.slope = slowHistory.back() - slowHistory[slowHistory.size() - 6];
    r.lastClose = bars.back().close;

    // Volatility: average true range of the last 14 bars.
    const std::size_t n = bars.size();
    const std::size_t atrBars = std::min<std::size_t>(14, n - 1);
    double trSum = 0.0;
    for (std::size_t i = n - atrBars; i < n; ++i) {
        const Bar& b = bars[i];
        const double prev = bars[i - 1].close;
        trSum += std::max({b.high - b.low, std::fabs(b.high - prev), std::fabs(b.low - prev)});
    }
    r.atr = trSum / static_cast<double>(atrBars);

    // Structure over the lookback: the later half's high and low vs the earlier half's.
    const std::size_t from = n - static_cast<std::size_t>(lookback);
    const std::size_t half = from + static_cast<std::size_t>(lookback) / 2;
    double h1 = bars[from].high, l1 = bars[from].low, h2 = bars[half].high, l2 = bars[half].low;
    for (std::size_t i = from; i < half; ++i) {
        h1 = std::max(h1, bars[i].high);
        l1 = std::min(l1, bars[i].low);
    }
    for (std::size_t i = half; i < n; ++i) {
        h2 = std::max(h2, bars[i].high);
        l2 = std::min(l2, bars[i].low);
    }
    r.recentHigh = std::max(h1, h2);
    r.recentLow = std::min(l1, l2);
    const int structure = (h2 > h1 && l2 > l1) ? 1 : ((h2 < h1 && l2 < l1) ? -1 : 0);

    const double flat = 0.05 * r.atr;
    const int emaOrder = fast > slow ? 1 : (fast < slow ? -1 : 0);
    const int slopeSign = r.slope > flat ? 1 : (r.slope < -flat ? -1 : 0);
    const int closeSide = r.lastClose > slow ? 1 : (r.lastClose < slow ? -1 : 0);
    r.score = emaOrder + slopeSign + closeSide + structure;
    if (r.atr > 0.0 && r.score >= 3) r.direction = TrendDirection::Up;
    if (r.atr > 0.0 && r.score <= -3) r.direction = TrendDirection::Down;

    auto word = [](int v, const char* up, const char* down, const char* none) { return v > 0 ? up : (v < 0 ? down : none); };
    r.why = std::string(r.direction == TrendDirection::Up     ? "UP"
                        : r.direction == TrendDirection::Down ? "DOWN"
                                                              : "no clear direction") +
            " (score " + (r.score > 0 ? "+" : "") + std::to_string(r.score) + "): EMA8 " +
            word(emaOrder, "above", "below", "at") + " EMA21, " + word(slopeSign, "rising", "falling", "flat") +
            ", price " + word(closeSide, "above", "below", "at") + " it, " +
            word(structure, "higher highs/lows", "lower highs/lows", "mixed highs/lows") + ", ATR " +
            fmt("%.2f", r.atr);
    return r;
}

}  // namespace muuk::core
