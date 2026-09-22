#include <filesystem>
#include <fstream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/backtester/tick_data_source.hpp"

using namespace muuk::backtester;

namespace {
std::filesystem::path fixturesDir() { return std::filesystem::path(MUUK_TEST_FIXTURES_DIR); }

std::filesystem::path writeTempCsv(const std::string& name, const std::string& contents) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::trunc);
    out << contents;
    out.close();
    return path;
}
}  // namespace

TEST_CASE("TickDataSource loads the sample fixture with a header row") {
    const auto records = TickDataSource::loadCsv(fixturesDir() / "sample_ticks.csv");
    REQUIRE(records.size() == 7);
    REQUIRE(records.front().timestampUnixMillis == 1700000000000);
    REQUIRE(records.back().timestampUnixMillis == 1700000006000);
    REQUIRE(records[1].bid == Catch::Approx(4337.09));
    REQUIRE(records[1].ask == Catch::Approx(4337.11));
}

TEST_CASE("TickDataSource loads a file with no header row") {
    const auto path = writeTempCsv("muuk_no_header.csv", "1,4336.00,4336.02\n2,4336.10,4336.12\n");
    const auto records = TickDataSource::loadCsv(path);
    REQUIRE(records.size() == 2);
    REQUIRE(records[0].timestampUnixMillis == 1);
}

TEST_CASE("TickDataSource rejects a malformed row") {
    const auto path = writeTempCsv("muuk_malformed.csv", "timestamp_ms,bid,ask\n1,not_a_number,4336.02\n");
    REQUIRE_THROWS_AS(TickDataSource::loadCsv(path), TickDataSourceError);
}

TEST_CASE("TickDataSource rejects out-of-order timestamps") {
    const auto path = writeTempCsv("muuk_unsorted.csv", "2,4336.10,4336.12\n1,4336.00,4336.02\n");
    REQUIRE_THROWS_AS(TickDataSource::loadCsv(path), TickDataSourceError);
}

TEST_CASE("TickDataSource throws for a missing file") {
    REQUIRE_THROWS_AS(TickDataSource::loadCsv("this_file_does_not_exist.csv"), TickDataSourceError);
}
