#include "muuk/ctrader_fix/market_hours.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#ifdef _MSC_VER
#define MUUK_SSCANF sscanf_s
#else
#define MUUK_SSCANF std::sscanf
#endif

namespace muuk::ctrader_fix {

namespace {

constexpr int64_t kMinute = 60'000;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int yearOf(int64_t days) {
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// 0 = Sunday .. 6 = Saturday.
int weekday(int64_t days) { return static_cast<int>(((days % 7) + 7 + 4) % 7); }

int64_t nthSunday(int y, unsigned m, int n) {
    const int64_t first = daysFromCivil(y, m, 1);
    return first + (7 - weekday(first)) % 7 + 7 * (n - 1);
}

int64_t lastSunday(int y, unsigned m) {
    const int64_t last = (m == 12 ? daysFromCivil(y + 1, 1, 1) : daysFromCivil(y, m + 1, 1)) - 1;
    return last - weekday(last);
}

struct SessionHours {
    int openHour;
    int closeHour;
};
constexpr std::array<SessionHours, kSessionCount> kHours{{{7, 16}, {9, 18}, {8, 17}, {8, 17}}};

struct LocalTime {
    int64_t day;     // local days since the epoch
    int weekday;     // 0 = Sunday
    int minute;      // minute of the local day
};

LocalTime localAt(int64_t utcMs, int offsetMinutes) {
    const int64_t local = utcMs + offsetMinutes * kMinute;
    const int64_t day = floorDiv(local, kDay);
    return {day, weekday(day), static_cast<int>((local - day * kDay) / kMinute)};
}

bool weekdayMonFri(int wd) { return wd >= 1 && wd <= 5; }

bool sessionOpen(Session s, int64_t utcMs) {
    const LocalTime lt = localAt(utcMs, sessionUtcOffsetMinutes(s, utcMs));
    const SessionHours h = kHours[static_cast<int>(s)];
    return weekdayMonFri(lt.weekday) && lt.minute >= h.openHour * 60 && lt.minute < h.closeHour * 60;
}

// XAUUSD: Sunday 18:00 to Friday 17:00 New York time, with a daily break 17:00-18:00.
bool weekendClosed(int64_t utcMs) {
    const LocalTime ny = localAt(utcMs, newYorkUtcOffsetMinutes(utcMs));
    return ny.weekday == 6 || (ny.weekday == 5 && ny.minute >= 17 * 60) || (ny.weekday == 0 && ny.minute < 18 * 60);
}

bool dailyBreak(int64_t utcMs) {
    const LocalTime ny = localAt(utcMs, newYorkUtcOffsetMinutes(utcMs));
    return ny.weekday >= 1 && ny.weekday <= 4 && ny.minute >= 17 * 60 && ny.minute < 18 * 60;
}

// Minutes since the market reopened at 18:00 New York (Sunday..Thursday), or -1.
int minutesSinceReopen(int64_t utcMs) {
    const LocalTime ny = localAt(utcMs, newYorkUtcOffsetMinutes(utcMs));
    if (ny.weekday > 4 || ny.minute < 18 * 60) return -1;
    return ny.minute - 18 * 60;
}

// Milliseconds until gold closes -- 17:00 New York, Monday to Friday (the
// daily break, or the weekend on Friday) -- or -1 when it's past that today.
int64_t msUntilClose(int64_t utcMs) {
    const int off = newYorkUtcOffsetMinutes(utcMs);
    const LocalTime ny = localAt(utcMs, off);
    if (!weekdayMonFri(ny.weekday) || ny.minute >= 17 * 60) return -1;
    const int64_t closeUtc = ny.day * kDay + 17 * kHour - off * kMinute;
    return closeUtc - utcMs;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " + ") + p;
    return out;
}


bool currencyListed(const std::string& list, const std::string& currency) {
    if (currency.empty()) return false;
    std::string token;
    for (std::size_t i = 0; i <= list.size(); ++i) {
        const char ch = i < list.size() ? list[i] : ',';
        if (ch == ',' || ch == ' ' || ch == ';') {
            if (!token.empty() && (token == currency || token == "ALL" || token == "*")) return true;
            token.clear();
        } else {
            token += static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        }
    }
    return false;
}

}  // namespace

bool newsIsGuarded(const NewsEvent& e, const MarketHoursConfig& c) {
    if (e.manual) return true;
    if (!currencyListed(c.newsCurrencies, e.currency)) return false;
    return (e.impact == "High" && c.newsHigh) || (e.impact == "Medium" && c.newsMedium);
}

int64_t utcMsFromCivil(int year, int month, int day, int hour, int minute, int second) {
    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * kDay + hour * kHour +
           minute * kMinute + second * int64_t{1000};
}

const char* sessionName(Session s) {
    switch (s) {
        case Session::Sydney: return "Sydney";
        case Session::Tokyo: return "Tokyo";
        case Session::London: return "London";
        case Session::NewYork: return "New York";
    }
    return "?";
}

const char* phaseName(MarketPhase p) {
    switch (p) {
        case MarketPhase::Open: return "open";
        case MarketPhase::Weekend: return "weekend";
        case MarketPhase::DailyBreak: return "daily break";
        case MarketPhase::SessionPause: return "session pause";
        case MarketPhase::NewsPause: return "news pause";
        case MarketPhase::OffHours: return "off hours";
    }
    return "?";
}

int newYorkUtcOffsetMinutes(int64_t utcMs) {
    // EDT from the 2nd Sunday of March 02:00 EST to the 1st Sunday of November 02:00 EDT.
    const int y = yearOf(floorDiv(utcMs, kDay));
    const int64_t start = nthSunday(y, 3, 2) * kDay + 7 * kHour;
    const int64_t end = nthSunday(y, 11, 1) * kDay + 6 * kHour;
    return utcMs >= start && utcMs < end ? -240 : -300;
}

int newYorkWeekday(int64_t utcMs) { return localAt(utcMs, newYorkUtcOffsetMinutes(utcMs)).weekday; }

int sessionUtcOffsetMinutes(Session s, int64_t utcMs) {
    const int y = yearOf(floorDiv(utcMs, kDay));
    switch (s) {
        case Session::Tokyo:
            return 540;
        case Session::London: {
            // BST from the last Sunday of March to the last Sunday of October, 01:00 UTC.
            const int64_t start = lastSunday(y, 3) * kDay + kHour;
            const int64_t end = lastSunday(y, 10) * kDay + kHour;
            return utcMs >= start && utcMs < end ? 60 : 0;
        }
        case Session::NewYork:
            return newYorkUtcOffsetMinutes(utcMs);
        case Session::Sydney: {
            // AEDT until the 1st Sunday of April 03:00 AEDT, and again from the
            // 1st Sunday of October 02:00 AEST -- both Saturday 16:00 UTC.
            const int64_t end = nthSunday(y, 4, 1) * kDay - 8 * kHour;
            const int64_t start = nthSunday(y, 10, 1) * kDay - 8 * kHour;
            return utcMs < end || utcMs >= start ? 660 : 600;
        }
    }
    return 0;
}

SessionWindow sessionWindow(Session s, int64_t utcMs) {
    const int off = sessionUtcOffsetMinutes(s, utcMs);
    const LocalTime lt = localAt(utcMs, off);
    const SessionHours h = kHours[static_cast<int>(s)];
    for (int k = 0; k < 8; ++k) {
        const int64_t day = lt.day + k;
        if (!weekdayMonFri(weekday(day))) continue;
        SessionWindow w;
        w.openUtcMs = day * kDay + h.openHour * kHour - off * kMinute;
        w.closeUtcMs = day * kDay + h.closeHour * kHour - off * kMinute;
        if (w.closeUtcMs <= utcMs) continue;
        w.openNow = utcMs >= w.openUtcMs;
        return w;
    }
    return {};
}

std::string sessionLabelAt(int64_t utcMs) {
    if (weekendClosed(utcMs) || dailyBreak(utcMs)) return "Market closed";
    std::vector<std::string> open;
    for (int i = 0; i < kSessionCount; ++i) {
        if (sessionOpen(static_cast<Session>(i), utcMs)) open.emplace_back(sessionName(static_cast<Session>(i)));
    }
    return open.empty() ? "Between sessions" : join(open);
}

MarketStatus evaluateMarket(int64_t utcMs, const MarketHoursConfig& c, const std::vector<NewsEvent>* feed,
                            bool findNext) {
    MarketStatus st;
    // The guarded release in force now, and the next one coming.
    const NewsEvent* news = nullptr;
    if (c.newsGuard) {
        auto consider = [&](const NewsEvent& n) {
            if (!newsIsGuarded(n, c)) return;
            const int64_t from = n.utcMs - c.newsMinutesBefore * kMinute;
            const int64_t to = n.utcMs + c.newsMinutesAfter * kMinute;
            if (utcMs >= from && utcMs < to && !news) news = &n;
            if (findNext && to > utcMs && (!st.hasNextNews || n.utcMs < st.nextNews.utcMs)) {
                st.hasNextNews = true;
                st.nextNews = n;
            }
        };
        for (const auto& n : c.news) consider(n);
        if (feed) {
            for (const auto& n : *feed) consider(n);
        }
    }
    for (int i = 0; i < kSessionCount; ++i) {
        if (sessionOpen(static_cast<Session>(i), utcMs)) st.sessions.emplace_back(sessionName(static_cast<Session>(i)));
    }

    auto set = [&](MarketPhase phase, std::string label, std::string detail) {
        st.phase = phase;
        st.label = std::move(label);
        st.detail = std::move(detail);
    };

    if (weekendClosed(utcMs)) {
        st.marketOpen = false;
        set(MarketPhase::Weekend, "Weekend - market closed",
            "Gold is closed from Friday 17:00 to Sunday 18:00 New York time.");
    } else if (dailyBreak(utcMs)) {
        st.marketOpen = false;
        set(MarketPhase::DailyBreak, "Daily break - market closed",
            "Gold's daily break, 17:00-18:00 New York time: no prices, orders wait.");
    } else if (!c.enabled) {
        set(MarketPhase::Open, "Open", "Market hours filter is off.");
    } else {
        // Only the market's own close and reopen pause new ranges -- the
        // trading sessions starting and ending don't.
        const int64_t toClose = msUntilClose(utcMs);
        const int sinceReopen = minutesSinceReopen(utcMs);
        if (news) {
            set(MarketPhase::NewsPause,
                "News guard: " + (news->currency.empty() ? "" : news->currency + " ") +
                    (news->title.empty() ? std::string("news") : news->title),
                "No new range from " + std::to_string(c.newsMinutesBefore) + " min before to " +
                    std::to_string(c.newsMinutesAfter) + " min after the release at " +
                    formatLocalDateTime(news->utcMs) + ".");
        } else if (c.pauseAroundClose && toClose >= 0 && toClose <= c.pauseMinutesBefore * kMinute) {
            set(MarketPhase::SessionPause, "Pause: market closing",
                "No new range in the last " + std::to_string(c.pauseMinutesBefore) +
                    " min before gold closes (17:00 New York: the daily break, or the weekend on Friday).");
        } else if (c.pauseAroundClose && sinceReopen >= 0 && sinceReopen < c.pauseMinutesAfter) {
            set(MarketPhase::SessionPause, "Pause: market reopening",
                "No new range in the first " + std::to_string(c.pauseMinutesAfter) +
                    " min after gold reopens (18:00 New York).");
        } else {
            set(MarketPhase::Open, "Open", "New ranges may open.");
        }
    }
    // Filter off: the bot ignores market hours altogether (as before they existed).
    st.canOpenRange = !c.enabled || st.phase == MarketPhase::Open;
    st.sessionLabel = st.marketOpen ? (st.sessions.empty() ? "Between sessions" : join(st.sessions)) : "Market closed";

    if (findNext && c.enabled) {
        // Minute by minute, up to a week: when does canOpenRange flip?
        const int64_t first = (floorDiv(utcMs, kMinute) + 1) * kMinute;
        for (int64_t t = first; t < utcMs + 8 * kDay; t += kMinute) {
            const MarketStatus next = evaluateMarket(t, c, feed, false);
            if (next.canOpenRange != st.canOpenRange) {
                st.nextChangeUtcMs = t;
                st.nextChangeLabel = next.canOpenRange ? "Trading resumes" : next.label;
                break;
            }
        }
    }
    return st;
}

nlohmann::json marketHoursToJson(const MarketHoursConfig& c) {
    nlohmann::json news = nlohmann::json::array();
    for (const auto& n : c.news) news.push_back({{"utcMs", n.utcMs}, {"title", n.title}, {"currency", n.currency}});
    return {{"enabled", c.enabled},
            {"newsGuard", c.newsGuard},
            {"newsHigh", c.newsHigh},
            {"newsMedium", c.newsMedium},
            {"newsCurrencies", c.newsCurrencies},
            {"pauseAroundClose", c.pauseAroundClose},
            {"pauseMinutesBefore", c.pauseMinutesBefore},
            {"pauseMinutesAfter", c.pauseMinutesAfter},
            {"news", news},
            {"newsMinutesBefore", c.newsMinutesBefore},
            {"newsMinutesAfter", c.newsMinutesAfter},
            {"closeRangeAt", c.closeRangeAt}};
}

MarketHoursConfig marketHoursFromJson(const nlohmann::json& j) {
    MarketHoursConfig c;
    if (!j.is_object()) return c;
    c.enabled = j.value("enabled", c.enabled);
    c.newsGuard = j.value("newsGuard", c.newsGuard);
    c.newsHigh = j.value("newsHigh", c.newsHigh);
    c.newsMedium = j.value("newsMedium", c.newsMedium);
    c.newsCurrencies = j.value("newsCurrencies", c.newsCurrencies);
    // Older files had per-session "tradeIn" / "pauseAtEndOf": no longer used
    // (every session is traded; only the market's close and reopen pause).
    c.pauseAroundClose = j.value("pauseAroundClose", c.pauseAroundClose);
    c.pauseMinutesBefore = std::max(0, j.value("pauseMinutesBefore", c.pauseMinutesBefore));
    c.pauseMinutesAfter = std::max(0, j.value("pauseMinutesAfter", c.pauseMinutesAfter));
    c.newsMinutesBefore = std::max(0, j.value("newsMinutesBefore", c.newsMinutesBefore));
    c.newsMinutesAfter = std::max(0, j.value("newsMinutesAfter", c.newsMinutesAfter));
    if (j.contains("news") && j["news"].is_array()) {
        for (const auto& n : j["news"]) {
            NewsEvent e;
            e.utcMs = n.value("utcMs", int64_t{0});
            e.title = n.value("title", std::string());
            e.currency = n.value("currency", std::string());
            e.impact = "High";
            e.manual = true;
            c.news.push_back(e);
        }
    }
    const std::string at = j.value("closeRangeAt", c.closeRangeAt);
    c.closeRangeAt = (at == "weekend" || at == "every") ? at : "never";
    return c;
}

int64_t utcNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool parseLocalDateTime(const std::string& text, int64_t& utcMs) {
    std::tm tm{};
    int sec = 0;
    const int n = MUUK_SSCANF(text.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                              &tm.tm_min, &sec);
    if (n < 5 || tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour < 0 ||
        tm.tm_hour > 23 || tm.tm_min < 0 || tm.tm_min > 59) {
        return false;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_sec = n >= 6 ? sec : 0;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) return false;
    utcMs = static_cast<int64_t>(t) * 1000;
    return true;
}

std::string formatLocalDateTime(int64_t utcMs) {
    const std::time_t t = static_cast<std::time_t>(utcMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

int64_t localTimestampToUtcMs(const std::string& text) {
    int64_t ms = 0;
    return parseLocalDateTime(text, ms) ? ms : 0;
}

}  // namespace muuk::ctrader_fix
