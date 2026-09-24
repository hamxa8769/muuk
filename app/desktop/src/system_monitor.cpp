#include "system_monitor.hpp"

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace muuk::desktop {

namespace {
#ifdef _WIN32
uint64_t toU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
#endif
}  // namespace

double SystemMonitor::appUptimeSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void SystemMonitor::sample(const std::filesystem::path& dataDir) {
    const auto now = std::chrono::steady_clock::now();
    if (havePrevious_ && now - lastSample_ < std::chrono::seconds(1)) return;
    lastSample_ = now;
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    latest_.cpuCores = static_cast<int>(si.dwNumberOfProcessors);

    // CPU: deltas of the system-wide idle/kernel/user times (kernel
    // includes idle), and of this process's kernel+user time against wall
    // time x cores.
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    FILETIME created{};
    FILETIME exited{};
    FILETIME pKernel{};
    FILETIME pUser{};
    FILETIME wall{};
    GetSystemTimeAsFileTime(&wall);
    if (GetSystemTimes(&idle, &kernel, &user) &&
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &pKernel, &pUser)) {
        const uint64_t i = toU64(idle);
        const uint64_t k = toU64(kernel);
        const uint64_t u = toU64(user);
        const uint64_t p = toU64(pKernel) + toU64(pUser);
        const uint64_t w = toU64(wall);
        if (havePrevious_) {
            const double busy = static_cast<double>((k - prevKernel_) + (u - prevUser_));
            const double idleD = static_cast<double>(i - prevIdle_);
            latest_.cpuSystemPct = busy > 0 ? 100.0 * (busy - idleD) / busy : 0.0;
            const double wallD = static_cast<double>(w - prevWall_) * std::max(1, latest_.cpuCores);
            latest_.cpuProcessPct = wallD > 0 ? 100.0 * static_cast<double>(p - prevProcess_) / wallD : 0.0;
            cpuHistory_.push_back(static_cast<float>(latest_.cpuSystemPct));
            while (cpuHistory_.size() > kHistory) cpuHistory_.pop_front();
        }
        prevIdle_ = i;
        prevKernel_ = k;
        prevUser_ = u;
        prevProcess_ = p;
        prevWall_ = w;
        havePrevious_ = true;
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        latest_.processMemoryMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        latest_.systemMemoryUsedPct = static_cast<double>(ms.dwMemoryLoad);
        latest_.systemMemoryTotalGb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }
    ULARGE_INTEGER freeBytes{};
    ULARGE_INTEGER totalBytes{};
    const std::wstring root = dataDir.root_path().wstring();
    if (GetDiskFreeSpaceExW(root.empty() ? nullptr : root.c_str(), &freeBytes, &totalBytes, nullptr)) {
        latest_.diskFreeGb = static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        latest_.diskTotalGb = static_cast<double>(totalBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
    }
    latest_.machineUptimeHours = static_cast<double>(GetTickCount64()) / 3'600'000.0;
#else
    (void)dataDir;
    havePrevious_ = true;
#endif
}

}  // namespace muuk::desktop
