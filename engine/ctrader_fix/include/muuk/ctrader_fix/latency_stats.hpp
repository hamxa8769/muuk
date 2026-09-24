#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace muuk::ctrader_fix {

struct LatencySummary {
    uint64_t samples = 0;  // all-time count
    double last = 0.0;
    double avg = 0.0;      // all-time mean
    double p50 = 0.0;      // over the most recent window
    double p99 = 0.0;      // over the most recent window
    double max = 0.0;      // all-time
};

// Running latency statistics in whatever unit the caller feeds it (the
// bot uses microseconds for its own decision time and milliseconds for
// broker round trips). O(1) per sample; percentiles come from a fixed
// window of the most recent samples, so memory stays constant however
// long the bot runs.
class LatencyTracker {
public:
    void add(double value) {
        window_[next_] = value;
        next_ = (next_ + 1) % kWindow;
        filled_ = std::min(filled_ + 1, kWindow);
        ++count_;
        sum_ += value;
        max_ = std::max(max_, value);
        last_ = value;
    }

    [[nodiscard]] LatencySummary summary() const {
        LatencySummary s;
        s.samples = count_;
        if (count_ == 0) return s;
        s.last = last_;
        s.avg = sum_ / static_cast<double>(count_);
        s.max = max_;
        if (filled_ == 0) return s;  // only carried-over totals so far
        std::array<double, kWindow> sorted{};
        std::copy_n(window_.begin(), filled_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(filled_));
        auto at = [&](double q) {
            const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(filled_ - 1) + 0.5);
            return sorted[std::min(i, filled_ - 1)];
        };
        s.p50 = at(0.50);
        s.p99 = at(0.99);
        return s;
    }

    // All-time totals, for saving across runs; carryOver() puts them back
    // (the percentile window starts empty again -- it's "recent" by design).
    struct Totals {
        uint64_t count = 0;
        double sum = 0.0;
        double max = 0.0;
    };
    [[nodiscard]] Totals totals() const { return Totals{count_, sum_, max_}; }
    void carryOver(const Totals& t) {
        count_ += t.count;
        sum_ += t.sum;
        max_ = std::max(max_, t.max);
    }
    void reset() { *this = LatencyTracker{}; }

private:
    static constexpr std::size_t kWindow = 2048;
    std::array<double, kWindow> window_{};
    std::size_t next_ = 0;
    std::size_t filled_ = 0;
    uint64_t count_ = 0;
    double sum_ = 0.0;
    double max_ = 0.0;
    double last_ = 0.0;
};

}  // namespace muuk::ctrader_fix
