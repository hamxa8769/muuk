#include "updater.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "version.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

namespace muuk::desktop {

namespace {

// Where releases come from, and the only place their Setup may be downloaded from.
constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/hamxa8769/muuk/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/hamxa8769/muuk/releases/download/";
constexpr const char* kSetupName = "Muuk-Setup.exe";
constexpr auto kCheckEvery = std::chrono::hours(6);
constexpr long long kMaxSetupBytes = 200LL * 1024 * 1024;

std::vector<int> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, '.')) {
        int n = 0;
        std::size_t i = 0;
        while (i < item.size() && std::isdigit(static_cast<unsigned char>(item[i]))) n = n * 10 + (item[i++] - '0');
        parts.push_back(n);
        if (i < item.size()) break;  // "1.2.0-beta": stop at the suffix
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// HTTPS GET (redirects followed, HTTPS only); each chunk goes to `sink`.
// `total` gets Content-Length when the server sends one.
bool httpsGet(const std::string& url, const std::function<bool(const char*, std::size_t)>& sink, long long& total,
              std::string& error) {
    total = 0;
    std::wstring wurl = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "not an https address";
        return false;
    }
    HINTERNET session = WinHttpOpen(L"Muuk-Updater/" MUUK_VERSION_STR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"Muuk-Updater/" MUUK_VERSION_STR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        error = "WinHTTP unavailable";
        return false;
    }
    WinHttpSetTimeouts(session, 15'000, 15'000, 30'000, 60'000);
    // Redirects (GitHub sends the file from its download host) only to HTTPS.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    bool ok = false;
    const std::wstring target = std::wstring(path) + extra;
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (request && WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0;
        DWORD size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &size, WINHTTP_NO_HEADER_INDEX);
        DWORD length = 0;
        size = sizeof(length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX)) {
            total = length;
        }
        if (code == 200) {
            ok = true;
            std::vector<char> chunk;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail)) {
                    ok = false;
                    error = "the download broke off";
                    break;
                }
                if (avail == 0) break;
                chunk.resize(avail);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) {
                    ok = false;
                    error = "the download broke off";
                    break;
                }
                if (read == 0) break;
                if (!sink(chunk.data(), read)) {
                    ok = false;
                    if (error.empty()) error = "the answer was too big";
                    break;
                }
            }
        } else {
            error = "GitHub answered HTTP " + std::to_string(code);
        }
    } else {
        error = "can't reach GitHub (error " + std::to_string(GetLastError()) + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// SHA-256 of a file, lowercase hex; empty on failure.
std::string fileSha256(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 16);
        bool ok = true;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0) != 0) {
                ok = false;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[3];
            for (unsigned char b : digest) {
                std::snprintf(hex, sizeof(hex), "%02x", b);
                out += hex;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}
#endif

}  // namespace

bool isNewerVersion(const std::string& candidate, const std::string& current) {
    const auto a = versionParts(candidate);
    const auto b = versionParts(current);
    for (std::size_t i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

Updater::~Updater() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Updater::start() {
    std::scoped_lock lock(mutex_);
    if (started_) return;
    started_ = true;
    checkRequested_ = true;
    thread_ = std::thread([this] { run(); });
}

void Updater::checkNow() {
    {
        std::scoped_lock lock(mutex_);
        if (status_.state == State::Downloading) return;
        checkRequested_ = true;
    }
    wake_.notify_all();
}

void Updater::download() {
    {
        std::scoped_lock lock(mutex_);
        if (setupUrl_.empty() || status_.state == State::Downloading || status_.state == State::Ready) return;
        downloadRequested_ = true;
    }
    wake_.notify_all();
}

Updater::Status Updater::status() const {
    std::scoped_lock lock(mutex_);
    return status_;
}

void Updater::set(State state, std::string message) {
    std::scoped_lock lock(mutex_);
    status_.state = state;
    status_.message = std::move(message);
}

void Updater::run() {
    auto nextCheck = std::chrono::steady_clock::now();
    for (;;) {
        bool doCheck = false;
        bool doDownload = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_until(lock, nextCheck, [this] { return stopping_ || checkRequested_ || downloadRequested_; });
            if (stopping_) return;
            doDownload = std::exchange(downloadRequested_, false);
            doCheck = std::exchange(checkRequested_, false) || std::chrono::steady_clock::now() >= nextCheck;
            // A Setup already downloaded stays: no need to ask again.
            if (status_.state == State::Ready) doCheck = false;
        }
        if (doDownload) fetchSetup();
        if (doCheck) {
            check();
            nextCheck = std::chrono::steady_clock::now() + kCheckEvery;
        }
    }
}

void Updater::check() {
#ifdef _WIN32
    {
        std::scoped_lock lock(mutex_);
        // Don't hide an offer already on screen while asking again.
        if (status_.state != State::Available) status_.state = State::Checking;
    }
    std::string body;
    long long total = 0;
    std::string error;
    const bool ok = httpsGet(
        kLatestReleaseUrl,
        [&](const char* data, std::size_t n) {
            body.append(data, n);
            return body.size() < 2 * 1024 * 1024;
        },
        total, error);
    if (!ok) {
        set(State::Failed, "Update check: " + error);
        return;
    }
    std::string tag, url, sha;
    long long size = 0;
    try {
        const auto j = nlohmann::json::parse(body);
        tag = j.value("tag_name", std::string());
        for (const auto& a : j.value("assets", nlohmann::json::array())) {
            if (a.value("name", std::string()) != kSetupName) continue;
            url = a.value("browser_download_url", std::string());
            const std::string digest = a.is_object() && a.contains("digest") && a["digest"].is_string()
                                           ? a["digest"].get<std::string>()
                                           : std::string();
            if (digest.rfind("sha256:", 0) == 0) sha = lower(digest.substr(7));
            size = a.value("size", 0LL);
        }
    } catch (const std::exception&) {
        set(State::Failed, "Update check: GitHub's answer couldn't be read");
        return;
    }
    if (tag.empty() || !isNewerVersion(tag, MUUK_VERSION_STR)) {
        std::scoped_lock lock(mutex_);
        status_.latest = tag;
        status_.state = State::UpToDate;
        status_.message = "Muuk " MUUK_VERSION_STR " is the newest version";
        return;
    }
    std::scoped_lock lock(mutex_);
    status_.latest = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    if (url.rfind(kDownloadPrefix, 0) != 0 || sha.size() != 64 || size <= 0 || size > kMaxSetupBytes) {
        // Without the file's SHA-256 from GitHub there is nothing to check it against.
        status_.state = State::Failed;
        status_.message = "Muuk " + status_.latest + " is out, but its Setup can't be checked -- download it from GitHub";
        setupUrl_.clear();
        return;
    }
    setupUrl_ = url;
    setupSha256_ = sha;
    setupSize_ = size;
    status_.state = State::Available;
    status_.message = "Muuk " + status_.latest + " is out (you have " MUUK_VERSION_STR ")";
#else
    set(State::Failed, "updates are only built on Windows");
#endif
}

void Updater::fetchSetup() {
#ifdef _WIN32
    std::string url, sha, version;
    long long expected = 0;
    {
        std::scoped_lock lock(mutex_);
        url = setupUrl_;
        sha = setupSha256_;
        expected = setupSize_;
        version = status_.latest;
        status_.state = State::Downloading;
        status_.progress = 0.0;
        status_.message = "Downloading Muuk " + version;
    }
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "Muuk-Update";
    std::filesystem::create_directories(dir, ec);
    const auto file = dir / ("Muuk-Setup-" + version + ".exe");
    long long got = 0;
    std::string error;
    bool ok = false;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            set(State::Failed, "Update: can't write to " + dir.string());
            return;
        }
        long long total = 0;
        ok = httpsGet(
            url,
            [&](const char* data, std::size_t n) {
                out.write(data, static_cast<std::streamsize>(n));
                got += static_cast<long long>(n);
                {
                    std::scoped_lock lock(mutex_);
                    if (stopping_) return false;
                    status_.progress = expected > 0 ? std::min(1.0, static_cast<double>(got) / expected) : 0.0;
                }
                return got <= kMaxSetupBytes && static_cast<bool>(out);
            },
            total, error);
    }
    if (!ok || got != expected) {
        std::filesystem::remove(file, ec);
        set(State::Failed, "Update download failed: " + (error.empty() ? std::string("the file is incomplete") : error));
        return;
    }
    if (fileSha256(file) != sha) {
        std::filesystem::remove(file, ec);
        set(State::Failed, "Update: the download doesn't match GitHub's SHA-256 -- not run");
        return;
    }
    std::scoped_lock lock(mutex_);
    setupPath_ = file.string();
    status_.state = State::Ready;
    status_.progress = 1.0;
    status_.message = "Muuk " + version + " is downloaded and checked";
#endif
}

bool Updater::launchSetup() {
#ifdef _WIN32
    std::string path;
    {
        std::scoped_lock lock(mutex_);
        if (status_.state != State::Ready) return false;
        path = setupPath_;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    return false;
#endif
}

}  // namespace muuk::desktop
