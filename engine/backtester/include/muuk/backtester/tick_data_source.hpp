#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace muuk::backtester {

// One row of historical price data. Schema documented in
// docs/data-format.md so the user can export matching data from a broker
// (e.g. MT5) later; this is intentionally the simplest format that can
// drive the strategy engine (bid/ask only — no volume/depth needed for
// this strategy).
struct TickRecord {
    int64_t timestampUnixMillis = 0;
    double bid = 0.0;
    double ask = 0.0;
};

class TickDataSourceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TickDataSource {
public:
    // Loads a CSV file with header `timestamp_ms,bid,ask` (header optional
    // — auto-detected by trying to parse the first row as data). Throws
    // TickDataSourceError on a malformed file. Rows must be sorted
    // ascending by timestamp; this is validated.
    [[nodiscard]] static std::vector<TickRecord> loadCsv(const std::filesystem::path& path);
};

}  // namespace muuk::backtester
