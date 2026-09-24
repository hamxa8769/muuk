#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "muuk/ctrader_fix/market_hours.hpp"

namespace muuk::ctrader_fix {

// ForexFactory's public weekly calendar (the JSON export of
// forexfactory.com/calendar). ForexFactory asks for no more than a couple
// of downloads every few minutes, so it's fetched at most once an hour and
// kept in a cache file that a restart reads first.
inline constexpr const char* kForexFactoryHost = "nfs.faireconomy.media";
inline constexpr const char* kForexFactoryPath = "/ff_calendar_thisweek.json";

// Parses the calendar JSON: [{"title","country","date":"2026-09-25T08:30:00-04:00",
// "impact","forecast","previous"}, ...]. Unparseable entries are skipped.
[[nodiscard]] std::vector<NewsEvent> parseForexFactoryCalendar(const std::string& json);

// Background downloader. Thread-safe; events() never blocks on the network.
class NewsFeed {
public:
    explicit NewsFeed(std::filesystem::path cacheFile);
    ~NewsFeed();
    NewsFeed(const NewsFeed&) = delete;
    NewsFeed& operator=(const NewsFeed&) = delete;

    void start();  // loads the cache, then refreshes in the background
    void stop();
    void refreshNow();  // skip the wait (still at most one download per 5 min)

    struct Status {
        bool loaded = false;        // some calendar is available
        std::string updatedAt;      // local time of the last successful download, "" = only the cache
        std::string error;          // last failure, "" = fine
        std::size_t events = 0;
    };
    [[nodiscard]] std::vector<NewsEvent> events() const;
    [[nodiscard]] Status status() const;

private:
    void loop();
    bool download(std::string& body, std::string& error) const;

    std::filesystem::path cacheFile_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<NewsEvent> events_;
    Status status_;
    int64_t fetchedAtMs_ = 0;       // when the calendar in hand was downloaded
    int64_t lastAttemptMs_ = 0;
    bool refreshRequested_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace muuk::ctrader_fix
