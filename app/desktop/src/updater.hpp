#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace muuk::desktop {

// True when `candidate` ("1.2.0" or "v1.2.0") is a newer version than `current`.
[[nodiscard]] bool isNewerVersion(const std::string& candidate, const std::string& current);

// App updates from the project's GitHub releases. A background thread asks
// GitHub for the newest release when the app opens and every 6 hours; when its
// tag is newer than this build, the header offers it. download() fetches its
// Muuk-Setup.exe to the temp folder and checks it against the SHA-256 GitHub
// publishes for that file -- a release without one is never offered. The
// downloaded Setup is started only after the app has exited (launchSetup()),
// because Setup refuses to replace a running Muuk.
class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    struct Status {
        State state = State::Idle;
        std::string latest;     // the newest release's version, once known
        std::string message;    // what failed, or what was found
        double progress = 0.0;  // 0..1 while downloading
    };

    Updater() = default;
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    ~Updater();

    void start();      // once; the first check runs at once
    void checkNow();   // ask GitHub again
    void download();   // only when Available (or a failed download)
    [[nodiscard]] Status status() const;
    // Starts the downloaded, checked Setup. Call after the app stopped trading.
    bool launchSetup();

private:
    void run();
    void check();
    void fetchSetup();
    void set(State state, std::string message);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool started_ = false;
    bool stopping_ = false;
    bool checkRequested_ = false;
    bool downloadRequested_ = false;
    Status status_;
    std::string setupUrl_;
    std::string setupSha256_;  // lowercase hex
    long long setupSize_ = 0;
    std::string setupPath_;    // the checked download
};

}  // namespace muuk::desktop
