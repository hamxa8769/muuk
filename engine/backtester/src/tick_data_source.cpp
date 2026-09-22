#include "muuk/backtester/tick_data_source.hpp"

#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace muuk::backtester {

namespace {

std::vector<std::string_view> splitCsvLine(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t comma = line.find(',', start);
        const std::size_t end = (comma == std::string_view::npos) ? line.size() : comma;
        fields.push_back(line.substr(start, end - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return fields;
}

bool tryParseRow(std::string_view line, TickRecord& out) {
    const auto fields = splitCsvLine(line);
    if (fields.size() != 3) return false;

    auto parseI64 = [](std::string_view s, int64_t& value) {
        const auto res = std::from_chars(s.data(), s.data() + s.size(), value);
        return res.ec == std::errc{} && res.ptr == s.data() + s.size();
    };
    auto parseDouble = [](std::string_view s, double& value) {
        // from_chars for double is not universally available across all
        // targeted toolchains yet; std::stod is used at this I/O boundary
        // only (never on the hot path).
        try {
            std::size_t consumed = 0;
            value = std::stod(std::string(s), &consumed);
            return consumed == s.size();
        } catch (...) {
            return false;
        }
    };

    int64_t ts = 0;
    double bid = 0.0;
    double ask = 0.0;
    if (!parseI64(fields[0], ts)) return false;
    if (!parseDouble(fields[1], bid)) return false;
    if (!parseDouble(fields[2], ask)) return false;

    out = TickRecord{ts, bid, ask};
    return true;
}

}  // namespace

std::vector<TickRecord> TickDataSource::loadCsv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw TickDataSourceError("cannot open tick data file: " + path.string());
    }

    std::vector<TickRecord> records;
    std::string line;
    bool firstLine = true;
    int lineNumber = 0;

    while (std::getline(file, line)) {
        ++lineNumber;
        if (line.empty()) continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        TickRecord record;
        const bool parsed = tryParseRow(line, record);

        if (firstLine) {
            firstLine = false;
            if (!parsed) {
                // Treat as a header row and skip it.
                continue;
            }
        }

        if (!parsed) {
            throw TickDataSourceError("malformed row " + std::to_string(lineNumber) + " in " + path.string());
        }

        if (!records.empty() && record.timestampUnixMillis < records.back().timestampUnixMillis) {
            throw TickDataSourceError("tick data is not sorted ascending by timestamp at row " +
                                       std::to_string(lineNumber) + " in " + path.string());
        }

        records.push_back(record);
    }

    return records;
}

}  // namespace muuk::backtester
