#include "muuk/ctrader_fix/instance_lock.hpp"

#include "muuk/ctrader_fix/live_state.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace muuk::ctrader_fix {

InstanceLock::~InstanceLock() { release(); }

bool InstanceLock::held() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

bool InstanceLock::acquire(const std::filesystem::path& lockFile) {
    if (held() && lockFile == file_) return true;
    release();
    std::error_code ec;
    if (lockFile.has_parent_path()) std::filesystem::create_directories(lockFile.parent_path(), ec);
#ifdef _WIN32
    // No sharing at all: a second open fails with a sharing violation while
    // this handle lives. Deleted automatically when the last handle closes.
    HANDLE h = CreateFileW(lockFile.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    handle_ = h;
#else
    const int fd = ::open(lockFile.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
#endif
    file_ = lockFile;
    return true;
}

void InstanceLock::release() {
#ifdef _WIN32
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
#else
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
}

std::filesystem::path instanceLockFile(const std::filesystem::path& configPath) {
    std::filesystem::path f = defaultStateFile(configPath);
    f.replace_extension(".lock");  // <config>.state.lock
    return f;
}

}  // namespace muuk::ctrader_fix
