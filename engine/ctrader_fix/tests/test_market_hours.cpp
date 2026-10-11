#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/market_hours.hpp"
#include "muuk/ctrader_fix/news_feed.hpp"

using namespace muuk::ctrader_fix;

namespace {
int64_t utc(int y, int mo, int d, int h, int mi) { return utcMsFromCivil(y, mo, d, h, mi, 0); }
MarketStatus at(int y, int mo, int d, int h, int mi, const MarketHoursConfig& c = {}) {
    return evaluateMarket(utc(y, mo, d, h, mi), c, nullptr, false);
}
}  // namespace

TEST_CASE("Daylight saving: New York, London and Sydney switch on the right days") {
    // 2026: US 8 Mar / 1 Nov, UK 29 Mar / 25 Oct, Sydney ends 5 Apr, starts 4 Oct.
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 3, 8, 6, 59)) == -300);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 3, 8, 7, 0)) == -240);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 11, 1, 5, 59)) == -240);
    REQUIRE(newYorkUtcOffsetMinutes(utc(2026, 11, 1, 6, 0)) == -300);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 3, 29, 0, 59)) == 0);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 3, 29, 1, 0)) == 60);
    REQUIRE(sessionUtcOffsetMinutes(Session::London, utc(2026, 10, 25, 1, 0)) == 0);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 4, 4, 15, 59)) == 660);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 4, 4, 16, 0)) == 600);
    REQUIRE(sessionUtcOffsetMinutes(Session::Sydney, utc(2026, 10, 3, 16, 0)) == 660);
    REQUIRE(sessionUtcOffsetMinutes(Session::Tokyo, utc(2026, 7, 1, 0, 0)) == 540);
    REQUIRE(newYorkWeekday(utc(2026, 9, 25, 12, 0)) == 5);  // Friday
}

TEST_CASE("Sessions open on their own city's clock") {
    // Thursday 24 Sep 2026 (summer): London 07-16 UTC, New York 12-21 UTC.
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 13, 0)) == "London + New York");
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 3, 0)) == "Sydney + Tokyo");
    REQUIRE(sessionLabelAt(utc(2026, 9, 24, 21, 30)) == "Market closed");  // daily break
    REQUIRE(sessionLabelAt(utc(2026, 9, 26, 12, 0)) == "Market closed");   // Saturday
}

TEST_CASE("Sessions ending don't pause anything -- only gold's own close and reopen do") {
    // London closes 17:00 BST = 16:00 UTC, Tokyo 18:00 JST = 09:00 UTC: trading goes on.
    REQUIRE(at(2026, 9, 24, 15, 55).phase == MarketPhase::Open);
    REQUIRE(at(2026, 9, 23, 8, 55).phase == MarketPhase::Open);
    // Gold closes 17:00 EDT = 21:00 UTC: no new range in the 15 min before.
    REQUIRE(at(2026, 9, 24, 20, 44).phase == MarketPhase::Open);
    const MarketStatus p = at(2026, 9, 24, 20, 50);
    REQUIRE(p.phase == MarketPhase::SessionPause);
    REQUIRE(p.label == "Pause: market closing");
    REQUIRE_FALSE(p.canOpenRange);
    // Switched off: no pause before the close.
    MarketHoursConfig c;
    c.pauseAroundClose = false;
    REQUIRE(at(2026, 9, 24, 20, 50, c).phase == MarketPhase::Open);
    REQUIRE(at(2026, 9, 24, 22, 0, c).phase == MarketPhase::Open);  // nor right after the reopen
}

TEST_CASE("Gold's daily break, the reopening minute and the weekend") {
    // New York close 17:00 EDT = 21:00 UTC: pause before, then the break.
    REQUIRE(at(2026, 9, 24, 20, 50).label == "Pause: market closing");
    const MarketStatus brk = at(2026, 9, 24, 21, 30);
    REQUIRE(brk.phase == MarketPhase::DailyBreak);
    REQUIRE_FALSE(brk.marketOpen);
    REQUIRE(at(2026, 9, 24, 22, 0).label == "Pause: market reopening");
    REQUIRE(at(2026, 9, 24, 22, 1).phase == MarketPhase::Open);
    // Winter: 17:00 EST = 22:00 UTC.
    REQUIRE(at(2026, 12, 15, 22, 30).phase == MarketPhase::DailyBreak);
    REQUIRE(at(2026, 12, 15, 21, 30).phase == MarketPhase::Open);
    // Friday 17:00 New York to Sunday 18:00.
    REQUIRE(at(2026, 9, 25, 21, 0).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 26, 12, 0).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 27, 21, 59).phase == MarketPhase::Weekend);
    REQUIRE(at(2026, 9, 27, 22, 1).phase == MarketPhase::Open);
    // Filter off: never blocks, but still says what the market is doing.
    MarketHoursConfig off;
    off.enabled = false;
    const MarketStatus wk = at(2026, 9, 26, 12, 0, off);
    REQUIRE(wk.phase == MarketPhase::Weekend);
    REQUIRE(wk.canOpenRange);
}

TEST_CASE("Every session is traded, also between sessions") {
    REQUIRE(at(2026, 9, 24, 3, 0).phase == MarketPhase::Open);    // Sydney + Tokyo
    REQUIRE(at(2026, 9, 24, 10, 0).phase == MarketPhase::Open);   // London
    REQUIRE(at(2026, 9, 24, 23, 30).phase == MarketPhase::Open);  // between New York and Sydney
    // Friday: the pause before the weekend close.
    REQUIRE(at(2026, 9, 25, 20, 50).label == "Pause: market closing");
}

TEST_CASE("The next change is found, with its reason") {
    const MarketStatus s = evaluateMarket(utc(2026, 9, 24, 15, 0), MarketHoursConfig{});
    REQUIRE(s.canOpenRange);
    REQUIRE(s.nextChangeUtcMs == utc(2026, 9, 24, 20, 45));
    REQUIRE(s.nextChangeLabel == "Pause: market closing");
    const MarketStatus w = evaluateMarket(utc(2026, 9, 26, 12, 0), MarketHoursConfig{});
    REQUIRE(w.nextChangeUtcMs == utc(2026, 9, 27, 22, 1));
    REQUIRE(w.nextChangeLabel == "Trading resumes");
}

TEST_CASE("ForexFactory calendar: parsed to UTC, and the news guard follows impact, currency and the toggle") {
    const std::string json = R"([
        {"title":"Non-Farm Employment Change","country":"USD","date":"2026-10-02T08:30:00-04:00","impact":"High","forecast":"150K","previous":"142K"},
        {"title":"ISM Services PMI","country":"USD","date":"2026-10-05T10:00:00-04:00","impact":"Medium","forecast":"","previous":""},
        {"title":"German CPI","country":"EUR","date":"2026-10-02T08:00:00+02:00","impact":"High","forecast":"","previous":""},
        {"title":"Bank Holiday","country":"GBP","date":"2026-10-02T03:00:00-04:00","impact":"Holiday","forecast":"","previous":""},
        {"title":"broken","country":"USD","date":"soon","impact":"High"}
    ])";
    const std::vector<NewsEvent> feed = parseForexFactoryCalendar(json);
    REQUIRE(feed.size() == 4);
    REQUIRE(feed[0].utcMs == utc(2026, 10, 2, 12, 30));
    REQUIRE(feed[0].impact == "High");
    REQUIRE(feed[0].forecast == "150K");
    REQUIRE(feed[2].utcMs == utc(2026, 10, 2, 6, 0));

    MarketHoursConfig c;  // guard on, High only, USD
    REQUIRE(newsIsGuarded(feed[0], c));
    REQUIRE_FALSE(newsIsGuarded(feed[1], c));  // Medium: off by default
    REQUIRE_FALSE(newsIsGuarded(feed[2], c));  // EUR
    c.newsMedium = true;
    REQUIRE(newsIsGuarded(feed[1], c));
    c.newsMedium = false;

    // NFP 12:30 UTC: guarded 12:15 .. 12:45.
    const MarketStatus before = evaluateMarket(utc(2026, 10, 2, 12, 10), c, &feed);
    REQUIRE(before.canOpenRange);
    REQUIRE(before.hasNextNews);
    REQUIRE(before.nextNews.title == "Non-Farm Employment Change");
    REQUIRE(before.nextChangeUtcMs == utc(2026, 10, 2, 12, 15));
    const MarketStatus during = evaluateMarket(utc(2026, 10, 2, 12, 20), c, &feed);
    REQUIRE(during.phase == MarketPhase::NewsPause);
    REQUIRE(during.label == "News guard: USD Non-Farm Employment Change");
    REQUIRE(evaluateMarket(utc(2026, 10, 2, 12, 45), c, &feed, false).phase == MarketPhase::Open);

    c.newsGuard = false;
    REQUIRE(evaluateMarket(utc(2026, 10, 2, 12, 20), c, &feed, false).phase == MarketPhase::Open);
}

TEST_CASE("Market hours settings survive a save and load") {
    MarketHoursConfig c;
    c.pauseAroundClose = false;
    c.pauseMinutesBefore = 20;
    c.newsGuard = false;
    c.newsMedium = true;
    c.newsCurrencies = "USD, EUR";
    c.closeRangeAt = "weekend";
    NewsEvent n;
    n.utcMs = utc(2026, 10, 2, 12, 30);
    n.title = "FOMC";
    n.manual = true;
    c.news.push_back(n);
    const MarketHoursConfig back = marketHoursFromJson(marketHoursToJson(c));
    REQUIRE_FALSE(back.pauseAroundClose);
    REQUIRE(back.pauseMinutesBefore == 20);
    REQUIRE_FALSE(back.newsGuard);
    REQUIRE(back.newsMedium);
    REQUIRE(back.newsCurrencies == "USD, EUR");
    REQUIRE(back.closeRangeAt == "weekend");
    REQUIRE(back.news.size() == 1);
    REQUIRE(back.news[0].manual);
    REQUIRE(back.news[0].title == "FOMC");
    // A hand-added event is always guarded (while the guard is on).
    REQUIRE(newsIsGuarded(back.news[0], MarketHoursConfig{}));
}
