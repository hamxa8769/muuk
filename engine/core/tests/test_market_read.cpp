#include <cmath>
#include <deque>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/market_read.hpp"

using namespace muuk::core;

namespace {

// `count` one-minute bars whose closes follow `price(i)`, each 0.30 tall.
template <typename F>
std::deque<Bar> bars(int count, F price) {
    std::deque<Bar> out;
    double prev = price(0);
    for (int i = 0; i < count; ++i) {
        const double c = price(i);
        out.push_back(Bar{i * 60'000LL, prev, std::max(prev, c) + 0.15, std::min(prev, c) - 0.15, c});
        prev = c;
    }
    return out;
}

}  // namespace

TEST_CASE("Market read: a steady climb is UP, a steady fall is DOWN") {
    const MarketRead up = readMarket(bars(40, [](int i) { return 4100.0 + 0.25 * i; }));
    REQUIRE(up.enoughData);
    REQUIRE(up.direction == TrendDirection::Up);
    REQUIRE(up.score == 4);
    REQUIRE(up.atr > 0.0);
    REQUIRE(up.recentHigh > up.recentLow);

    const MarketRead down = readMarket(bars(40, [](int i) { return 4100.0 - 0.25 * i; }));
    REQUIRE(down.direction == TrendDirection::Down);
    REQUIRE(down.score == -4);
}

TEST_CASE("Market read: a sideways chop has no clear direction -- no escape is taken into it") {
    const MarketRead chop = readMarket(bars(40, [](int i) { return 4100.0 + ((i % 2) ? 0.6 : -0.6); }));
    REQUIRE(chop.enoughData);
    REQUIRE(chop.direction == TrendDirection::None);
    REQUIRE(std::abs(chop.score) < 3);
}

TEST_CASE("Market read: too few bars -> not enough data, no direction") {
    const MarketRead r = readMarket(bars(10, [](int i) { return 4100.0 + i; }));
    REQUIRE_FALSE(r.enoughData);
    REQUIRE(r.direction == TrendDirection::None);
    REQUIRE(r.why.find("10 of 22") != std::string::npos);
}

TEST_CASE("Bar series: quotes make minute bars; the forming one isn't closed yet; history seeds older bars") {
    BarSeries s(60'000);
    s.addQuote(0, 4100.0);
    s.addQuote(30'000, 4101.0);
    s.addQuote(59'000, 4099.5);
    REQUIRE(s.closedCount() == 0);  // still forming
    s.addQuote(60'000, 4100.5);     // the next minute starts
    REQUIRE(s.closedCount() == 1);
    const Bar first = s.closed().front();
    REQUIRE(first.open == Catch::Approx(4100.0));
    REQUIRE(first.high == Catch::Approx(4101.0));
    REQUIRE(first.low == Catch::Approx(4099.5));
    REQUIRE(first.close == Catch::Approx(4099.5));

    s.addClosed(Bar{-60'000, 4098.0, 4098.5, 4097.5, 4098.2});  // an older bar from history
    REQUIRE(s.closedCount() == 2);
    REQUIRE(s.closed().front().startMs == -60'000);
}
