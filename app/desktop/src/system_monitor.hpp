#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>

namespace muuk::desktop {

// Machine and process health for the Performance tab -- what matters when
// the bot runs on a VPS: is the CPU or memory maxed out, is the disk
// filling up, how long has the machine been up. Sampled at most once a
// second (sample() is cheap to call every frame). Windows only; elsewhere
// everything reads 0.
class SystemMonitor {
public:
    struct Sample {
        double cpuSystemPct = 0.0;   // whole machine, all cores
        double cpuProcessPct = 0.0;  // this app, as a share of the whole machine
        double processMemoryMb = 0.0;
        double systemMemoryUsedPct = 0.0;
        double systemMemoryTotalGb = 0.0;
        double diskFreeGb = 0.0;     // on the drive holding the data folder
        double diskTotalGb = 0.0;
        double machineUptimeHours = 0.0;
        int cpuCores = 0;
    };

    // Samples if a second has passed since the last one. `dataDir` picks the
    // drive for the disk figures.
    void sample(const std::filesystem::path& dataDir);

    [[nodiscard]] const Sample& latest() const { return latest_; }
    // The last 5 minutes, one value per second, oldest first.
    [[nodiscard]] const std::deque<float>& cpuHistory() const { return cpuHistory_; }
    [[nodiscard]] double appUptimeSeconds() const;

    static constexpr std::size_t kHistory = 300;

private:
    Sample latest_;
    std::deque<float> cpuHistory_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastSample_{};
    bool havePrevious_ = false;
    uint64_t prevIdle_ = 0;
    uint64_t prevKernel_ = 0;
    uint64_t prevUser_ = 0;
    uint64_t prevProcess_ = 0;
    uint64_t prevWall_ = 0;
};

}  // namespace muuk::desktop
