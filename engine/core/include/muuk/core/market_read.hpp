#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace muuk::core {

// One bar of mid prices (a minute by default).
struct Bar {
    int64_t startMs = 0;  // UTC ms the bar started
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
};

// Bars built from quotes as they arrive; the newest bar is still forming.
class BarSeries {
public:
    explicit BarSeries(int64_t barMs = 60'000, std::size_t keep = 240) : barMs_(barMs), keep_(keep) {}
    void addQuote(int64_t utcMs, double mid);
    // Bars that are finished (all but the one still forming), oldest first.
    [[nodiscard]] std::deque<Bar> closed() const;
    [[nodiscard]] std::size_t closedCount() const;
    // Seeds finished bars (e.g. from the broker's chart history), oldest first.
    void addClosed(const Bar& bar);
    [[nodiscard]] int64_t barMs() const { return barMs_; }

private:
    int64_t barMs_;
    std::size_t keep_;
    std::deque<Bar> bars_;
    bool lastIsForming_ = false;  // the newest bar is still taking quotes
};

enum class TrendDirection { None, Up, Down };

// What the price itself says about the market: trend (fast vs slow EMA and
// the slow one's slope), structure (higher highs and lows, or lower ones),
// volatility (ATR) and the recent extremes a breakout must clear. There is
// no order book or sentiment feed here -- only what the quotes show.
struct MarketRead {
    TrendDirection direction = TrendDirection::None;
    int score = 0;             // -4..+4: one point each for EMA order, slope, close vs slow EMA, structure
    double atr = 0.0;          // average true range of a bar
    double emaFast = 0.0;
    double emaSlow = 0.0;
    double slope = 0.0;        // slow EMA now minus 5 bars ago
    double recentHigh = 0.0;   // over the lookback
    double recentLow = 0.0;
    double lastClose = 0.0;
    int bars = 0;              // closed bars used
    bool enoughData = false;
    std::string why;           // one line, for the operator
};

// Needs at least max(lookback, 22) closed bars; fewer -> enoughData false.
// A direction only when at least 3 of the 4 signals agree.
[[nodiscard]] MarketRead readMarket(const std::deque<Bar>& closedBars, int lookback = 15);

}  // namespace muuk::core
