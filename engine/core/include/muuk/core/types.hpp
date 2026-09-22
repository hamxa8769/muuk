#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <string>

namespace muuk::core {

// --- SymbolSpec ---------------------------------------------------------
// Broker/instrument specification. Everything the engine needs to convert
// between human-readable prices/volumes and the integer tick/step units it
// actually computes with (see Price / Volume below). Proposal §5.
struct SymbolSpec {
    std::string name = "XAUUSD";
    int digits = 2;              // decimal digits in quoted price
    double tickSize = 0.01;      // smallest price increment (1 point)
    double tickValue = 1.0;      // monetary value of 1 tick for 1.0 lot
    double contractSize = 100.0; // units of base asset per 1.0 lot
    double volumeStep = 0.01;    // smallest volume increment
    double volumeMin = 0.01;
    double volumeMax = 100.0;

    [[nodiscard]] bool isValid() const {
        return digits >= 0 && tickSize > 0.0 && tickValue > 0.0 &&
               contractSize > 0.0 && volumeStep > 0.0 && volumeMin > 0.0 &&
               volumeMax >= volumeMin;
    }
};

namespace detail {
// Rounds to the nearest integer, half away from zero — deterministic and
// independent of the current floating-point rounding mode.
inline int64_t roundToInt64(double value) {
    return static_cast<int64_t>(value >= 0.0 ? std::floor(value + 0.5)
                                              : std::ceil(value - 0.5));
}
}  // namespace detail

// --- Price ---------------------------------------------------------------
// Wraps an absolute price as a count of ticks (SymbolSpec::tickSize units).
// No `double` participates in engine decision logic; conversion to/from a
// human-readable value only happens at I/O boundaries (config, CSV, logs).
struct Price {
    int64_t ticks = 0;

    static Price fromDouble(double price, const SymbolSpec& spec) {
        return Price{detail::roundToInt64(price / spec.tickSize)};
    }

    [[nodiscard]] double toDouble(const SymbolSpec& spec) const {
        return static_cast<double>(ticks) * spec.tickSize;
    }

    friend auto operator<=>(const Price&, const Price&) = default;
};

inline Price operator+(Price a, Price b) { return Price{a.ticks + b.ticks}; }
inline Price operator-(Price a, Price b) { return Price{a.ticks - b.ticks}; }

// --- Volume ----------------------------------------------------------------
// Wraps a lot size as a count of SymbolSpec::volumeStep units.
struct Volume {
    int64_t steps = 0;

    static Volume fromDouble(double lots, const SymbolSpec& spec) {
        return Volume{detail::roundToInt64(lots / spec.volumeStep)};
    }

    [[nodiscard]] double toDouble(const SymbolSpec& spec) const {
        return static_cast<double>(steps) * spec.volumeStep;
    }

    // Clamp to [spec.volumeMin, spec.volumeMax], already expressed in steps.
    [[nodiscard]] Volume clampedTo(Volume min, Volume max) const {
        if (steps < min.steps) return min;
        if (steps > max.steps) return max;
        return *this;
    }

    friend auto operator<=>(const Volume&, const Volume&) = default;
};

inline Volume operator+(Volume a, Volume b) { return Volume{a.steps + b.steps}; }

}  // namespace muuk::core
