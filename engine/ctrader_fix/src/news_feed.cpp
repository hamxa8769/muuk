#include "muuk/ctrader_fix/news_feed.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

#ifdef _MSC_VER
#define MUUK_SSCANF sscanf_s
#else
#define MUUK_SSCANF std::sscanf
#endif

#include <nlohmann/json.hpp>

#include "muuk/ctrader_fix/basket_history.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace muuk::ctrader_fix {

namespace {
constexpr int64_t kRefreshMs = 60 * 60'000;       // once an hour
constexpr int64_t kRetryMs = 10 * 60'000;         // after a failed download
constexpr int64_t kMinGapMs = 5 * 60'000;         // never more often than this
}  // namespace

std::vector<NewsEvent> parseForexFactoryCalendar(const std::string& json) {
    std::vector<NewsEvent> out;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        const std::string date = e.value("date", std::string());
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, oh = 0, om = 0;
        // 2026-09-25T08:30:00-04:00
        if (date.size() < 25 ||
            MUUK_SSCANF(date.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6 ||
            MUUK_SSCANF(date.c_str() + 20, "%d:%d", &oh, &om) != 2) {
            continue;
        }
        const char sign = date[19];
        if (sign != '+' && sign != '-') continue;
        const int offsetMinutes = (sign == '-' ? -1 : 1) * (oh * 60 + om);
        NewsEvent n;
        n.utcMs = utcMsFromCivil(y, mo, d, h, mi, s) - int64_t{offsetMinutes} * 60'000;
        n.title = e.value("title", std::string());
        n.currency = e.value("country", std::string());
        n.impact = e.value("impact", std::string());
        n.forecast = e.value("forecast", std::string());
        n.previous = e.value("previous", std::string());
        out.push_back(std::move(n));
    }
    return out;
}

NewsFeed::NewsFeed(std::filesystem::path cacheFile) : cacheFile_(std::move(cacheFile)) {}

NewsFeed::~NewsFeed() { stop(); }

void NewsFeed::start() {
    {
        std::scoped_lock lock(mutex_);
        if (thread_.joinable()) return;
        stopping_ = false;
        // The cache first: a restart shows the calendar at once, without a download.
        std::ifstream in(cacheFile_);
        if (in) {
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object() && j.contains("calendar")) {
                events_ = parseForexFactoryCalendar(j["calendar"].dump());
                fetchedAtMs_ = j.value("fetchedAtMs", int64_t{0});
                status_.loaded = !events_.empty();
                status_.events = events_.size();
                status_.updatedAt = j.value("fetchedAt", std::string());
            }
        }
    }
    thread_ = std::thread([this] { loop(); });
}

void NewsFeed::stop() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void NewsFeed::refreshNow() {
    {
        std::scoped_lock lock(mutex_);
        refreshRequested_ = true;
    }
    wake_.notify_all();
}

std::vector<NewsEvent> NewsFeed::events() const {
    std::scoped_lock lock(mutex_);
    return events_;
}

NewsFeed::Status NewsFeed::status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

void NewsFeed::loop() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        const int64_t now = utcNowMs();
        const bool failedLast = !status_.error.empty();
        const int64_t due = failedLast ? lastAttemptMs_ + kRetryMs : fetchedAtMs_ + kRefreshMs;
        const bool asked = refreshRequested_ && now - lastAttemptMs_ >= kMinGapMs;
        if (now >= due || asked) {
            refreshRequested_ = false;
            lastAttemptMs_ = now;
            lock.unlock();
            std::string body;
            std::string error;
            const bool ok = download(body, error);
            std::vector<NewsEvent> parsed;
            if (ok) {
                parsed = parseForexFactoryCalendar(body);
                if (parsed.empty()) error = "the calendar came back empty or unreadable";
            }
            lock.lock();
            if (ok && !parsed.empty()) {
                events_ = std::move(parsed);
                fetchedAtMs_ = now;
                status_.loaded = true;
                status_.events = events_.size();
                status_.updatedAt = localTimestamp();
                status_.error.clear();
                // Cache it (atomically: a crash mid-write keeps the old one).
                const nlohmann::json cache = {{"fetchedAtMs", fetchedAtMs_},
                                              {"fetchedAt", status_.updatedAt},
                                              {"calendar", nlohmann::json::parse(body, nullptr, false)}};
                std::error_code ec;
                if (!cacheFile_.parent_path().empty()) std::filesystem::create_directories(cacheFile_.parent_path(), ec);
                const auto tmp = cacheFile_.string() + ".tmp";
                {
                    std::ofstream out(tmp, std::ios::trunc);
                    out << cache.dump();
                }
                std::filesystem::rename(tmp, cacheFile_, ec);
            } else {
                status_.error = error.empty() ? "download failed" : error;
            }
            continue;
        }
        // Sleep until the next due time (or a refresh/stop request), at most a minute.
        const int64_t waitMs = std::clamp<int64_t>(due - now, 1'000, 60'000);
        wake_.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return stopping_ || refreshRequested_; });
        if (refreshRequested_ && utcNowMs() - lastAttemptMs_ < kMinGapMs) {
            // Too soon after the last download: honour it when the gap has passed.
            wake_.wait_for(lock, std::chrono::milliseconds(kMinGapMs - (utcNowMs() - lastAttemptMs_)),
                           [&] { return stopping_; });
        }
    }
}

bool NewsFeed::download(std::string& body, std::string& error) const {
#ifdef _WIN32
    HINTERNET session = WinHttpOpen(L"Muuk/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // Older Windows (before 8.1) has no automatic proxy.
        session = WinHttpOpen(L"Muuk/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        error = "WinHTTP unavailable";
        return false;
    }
    WinHttpSetTimeouts(session, 10'000, 10'000, 15'000, 15'000);
    bool ok = false;
    const std::wstring host(kForexFactoryHost, kForexFactoryHost + std::char_traits<char>::length(kForexFactoryHost));
    const std::wstring path(kForexFactoryPath, kForexFactoryPath + std::char_traits<char>::length(kForexFactoryPath));
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0;
        DWORD size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
                body.append(chunk.data(), read);
                if (body.size() > 4 * 1024 * 1024) break;  // not a calendar
            }
            ok = !body.empty();
            if (!ok) error = "empty answer from ForexFactory";
        } else {
            error = "ForexFactory answered HTTP " + std::to_string(code) +
                    (code == 429 ? " (too many requests -- will retry later)" : "");
        }
    } else {
        error = "can't reach ForexFactory (error " + std::to_string(GetLastError()) + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
#else
    (void)body;
    error = "news download is only built on Windows";
    return false;
#endif
}

}  // namespace muuk::ctrader_fix
