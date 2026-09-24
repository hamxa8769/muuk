#pragma once

#include <filesystem>

namespace muuk::ctrader_fix {

// One bot per config at a time. Two Muuk processes on the same login --
// e.g. the auto-start task plus a copy started by hand, or muuk-desktop
// plus muuk-live -- would keep kicking each other off the broker (a second
// FIX logon ends the first session) and both manage the same basket. The
// lock is an exclusively opened file next to the state file; the OS
// releases it when the process ends, even on a crash.
class InstanceLock {
public:
    InstanceLock() = default;
    ~InstanceLock();
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;

    // false if another process holds it. Re-acquiring the same file is a no-op.
    bool acquire(const std::filesystem::path& lockFile);
    void release();
    [[nodiscard]] bool held() const;
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }

private:
    std::filesystem::path file_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// The lock file for a config: next to its state file.
[[nodiscard]] std::filesystem::path instanceLockFile(const std::filesystem::path& configPath);

}  // namespace muuk::ctrader_fix
