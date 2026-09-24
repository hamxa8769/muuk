#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace muuk::ctrader_fix {

// The four FX trading sessions, each on its own city's clock (daylight
// saving included): Sydney 07-16, Tokyo 09-18, London 08-17, New York
// 08-17, Monday to Friday.
enum class Session { Sydney = 0, Tokyo = 1, London = 2, NewYork = 3 };
inline constexpr int kSessionCount = 4;
[[nodiscard]] const char* sessionName(Session s);

// A news release to stay out of, e.g. NFP -- from the ForexFactory
// calendar, or added by hand.
struct NewsEvent {
    int64_t utcMs = 0;     // release time
    std::string title;
    std::string currency;  // "USD"...
    std::string impact;    // "High" (red), "Medium" (orange), "Low", "Holiday"; manual entries: "High"
    std::string forecast;
    std::string previous;
    bool manual = false;
};

// When the bot may open a new range. A range already running keeps being
// managed through any pause (profit target and stop loss still work);
// only new ranges wait -- unless closeRangeAt says otherwise.
struct MarketHoursConfig {
    bool enabled = true;
    // Only open ranges while at least one of these sessions is open.
    std::array<bool, kSessionCount> tradeIn{true, true, true, true};
    // Pause from pauseMinutesBefore before to pauseMinutesAfter after the
    // end of each ticked session (spreads widen and prices jump there).
    std::array<bool, kSessionCount> pauseAtEndOf{true, true, true, true};
    int pauseMinutesBefore = 15;
    int pauseMinutesAfter = 1;
    // News guard: pause newsMinutesBefore .. newsMinutesAfter around each
    // guarded release -- ForexFactory events of the ticked impacts in the
    // listed currencies, plus the ones added by hand in `news`.
    bool newsGuard = true;
    bool newsHigh = true;       // red folder
    bool newsMedium = false;    // orange folder
    std::string newsCurrencies = "USD";  // comma separated; gold moves on USD news
    std::vector<NewsEvent> news;         // added by hand
    int newsMinutesBefore = 15;
    int newsMinutesAfter = 15;
    // "never": a running range is left to finish; "weekend": close it before
    // the Friday close; "every": close it whenever a pause starts.
    std::string closeRangeAt = "never";
};

[[nodiscard]] nlohmann::json marketHoursToJson(const MarketHoursConfig& c);
[[nodiscard]] MarketHoursConfig marketHoursFromJson(const nlohmann::json& j);

enum class MarketPhase {
    Open,          // new ranges may open
    Weekend,       // XAUUSD closed: Friday 17:00 to Sunday 18:00 New York time
    DailyBreak,    // XAUUSD's daily break, 17:00-18:00 New York time
    SessionPause,  // around a session's end
    NewsPause,     // around a news release
    OffHours,      // none of the chosen sessions is open
};
[[nodiscard]] const char* phaseName(MarketPhase p);

struct MarketStatus {
    MarketPhase phase = MarketPhase::Open;
    bool marketOpen = true;         // the broker quotes XAUUSD right now
    bool canOpenRange = true;       // phase == Open (or the filter is off and the market is open)
    std::string label;              // short, for a status pill: "Open", "Weekend", "Pause: London close"...
    std::string detail;             // one line of why
    std::vector<std::string> sessions;  // sessions open right now, e.g. {"London", "New York"}
    std::string sessionLabel;       // "London + New York", or "Between sessions"
    int64_t nextChangeUtcMs = 0;    // when canOpenRange flips next (0 = not within a week)
    std::string nextChangeLabel;    // "London close pause" / "Trading resumes"
    // The next guarded news release (or the one being guarded now).
    bool hasNextNews = false;
    NewsEvent nextNews;
};

// Does the news guard stop trading around this event?
[[nodiscard]] bool newsIsGuarded(const NewsEvent& e, const MarketHoursConfig& config);

// Pure: the market's state at `utcMs` under `config`. nextChange* is only
// filled in when findNext is true (it scans ahead, minute by minute).
// `feed` = the ForexFactory calendar (may be null).
[[nodiscard]] MarketStatus evaluateMarket(int64_t utcMs, const MarketHoursConfig& config,
                                          const std::vector<NewsEvent>* feed = nullptr, bool findNext = true);

// Which sessions are open at utcMs, as one label -- for grouping results
// ("London + New York", "Tokyo", "Between sessions", or "Market closed").
[[nodiscard]] std::string sessionLabelAt(int64_t utcMs);

// The session's window (UTC ms) that is open at utcMs, or else the next one.
struct SessionWindow {
    int64_t openUtcMs = 0;
    int64_t closeUtcMs = 0;
    bool openNow = false;
};
[[nodiscard]] SessionWindow sessionWindow(Session s, int64_t utcMs);

// UTC offset (minutes) of a session's city at utcMs, daylight saving included.
[[nodiscard]] int sessionUtcOffsetMinutes(Session s, int64_t utcMs);
[[nodiscard]] int newYorkUtcOffsetMinutes(int64_t utcMs);
// Day of the week in New York at utcMs, 0 = Sunday .. 5 = Friday.
[[nodiscard]] int newYorkWeekday(int64_t utcMs);

// UTC ms for a UTC civil date and time.
[[nodiscard]] int64_t utcMsFromCivil(int year, int month, int day, int hour, int minute, int second);

// Milliseconds since the epoch, now.
[[nodiscard]] int64_t utcNowMs();
// "YYYY-MM-DD HH:MM" in this computer's local time <-> UTC ms. Returns
// false on a malformed string.
[[nodiscard]] bool parseLocalDateTime(const std::string& text, int64_t& utcMs);
[[nodiscard]] std::string formatLocalDateTime(int64_t utcMs);
// "YYYY-MM-DD HH:MM:SS" local (as written in the history) -> UTC ms; 0 if malformed.
[[nodiscard]] int64_t localTimestampToUtcMs(const std::string& text);

}  // namespace muuk::ctrader_fix
