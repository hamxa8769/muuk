#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/ctrader_fix/basket_history.hpp"
#include "muuk/ctrader_fix/latency_stats.hpp"
#include "muuk/ctrader_fix/pdf_writer.hpp"
#include "muuk/ctrader_fix/session_report.hpp"
#include "muuk/ctrader_fix/volume_stats.hpp"

using namespace muuk::ctrader_fix;

namespace {

BasketRecord sampleBasket(int number, double net) {
    BasketRecord b;
    b.number = number;
    b.session = "2026-09-24 10:00:00";
    b.symbol = "XAUUSD";
    b.startedAt = "2026-09-24 10:0" + std::to_string(number) + ":00";
    b.closedAt = "2026-09-24 10:0" + std::to_string(number) + ":45";
    b.durationSeconds = 45.0;
    b.rangeLower = 4280.00;
    b.rangeUpper = 4282.00;
    b.closeReason = "profit target";
    BasketLegRecord buy{1, "P1", true, 0.01, 4282.00, 4282.05, 4283.10, 1.05, 0.07, 0.98, "t0", "t1", false};
    BasketLegRecord sell{2, "P2", false, 0.02, 4280.00, 4280.00, 4279.90, 0.20, 0.14, 0.06, "t0", "t1", false};
    b.legs = {buy, sell};
    b.gross = 1.25;
    b.commission = 0.21;
    b.net = net;
    return b;
}

// Checks the parts of a PDF a viewer depends on: header, trailer, and
// that startxref and every xref entry point at exactly the right bytes.
void requireWellFormedPdf(const std::string& pdf) {
    REQUIRE(pdf.rfind("%PDF-1.4", 0) == 0);
    REQUIRE(pdf.size() > 6);
    REQUIRE(pdf.substr(pdf.size() - 6) == "%%EOF\n");

    const std::size_t sx = pdf.rfind("startxref\n");
    REQUIRE(sx != std::string::npos);
    const std::size_t xrefOffset = std::stoul(pdf.substr(sx + 10));
    REQUIRE(pdf.compare(xrefOffset, 4, "xref") == 0);

    std::smatch m;
    const std::string afterXref = pdf.substr(xrefOffset);
    REQUIRE(std::regex_search(afterXref, m, std::regex("xref\\n0 (\\d+)\\n")));
    const int count = std::stoi(m[1]);
    std::size_t pos = xrefOffset + static_cast<std::size_t>(m.length(0)) + 20;  // skip the free entry
    for (int obj = 1; obj < count; ++obj) {
        const std::string entry = pdf.substr(pos, 20);
        REQUIRE(entry.size() == 20);
        const std::size_t off = std::stoul(entry.substr(0, 10));
        const std::string expected = std::to_string(obj) + " 0 obj";
        REQUIRE(pdf.compare(off, expected.size(), expected) == 0);
        pos += 20;
    }
}

}  // namespace

TEST_CASE("Basket history round-trips through its JSON Lines file and skips a torn line") {
    const auto path = std::filesystem::temp_directory_path() / "muuk_test_history.jsonl";
    std::filesystem::remove(path);

    appendBasketHistory(path, sampleBasket(1, 1.04));
    appendBasketHistory(path, sampleBasket(2, -0.50));
    {
        std::ofstream torn(path, std::ios::app);
        torn << "{\"number\": 3, \"legs\": [";  // e.g. power cut mid-write
    }

    const auto loaded = loadBasketHistory(path);
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].number == 1);
    REQUIRE(loaded[0].net == Catch::Approx(1.04));
    REQUIRE(loaded[0].closeReason == "profit target");
    REQUIRE(loaded[0].legs.size() == 2);
    REQUIRE(loaded[0].legs[0].buy);
    REQUIRE(loaded[0].legs[0].positionId == "P1");
    REQUIRE(loaded[0].legs[0].exitPrice == Catch::Approx(4283.10));
    REQUIRE_FALSE(loaded[0].legs[1].buy);
    REQUIRE(loaded[1].net == Catch::Approx(-0.50));

    REQUIRE(loadBasketHistory(std::filesystem::temp_directory_path() / "muuk_no_such_file.jsonl").empty());
    std::filesystem::remove(path);
}

TEST_CASE("LatencyTracker reports count, average, max and percentiles") {
    LatencyTracker t;
    REQUIRE(t.summary().samples == 0);
    for (int i = 1; i <= 100; ++i) t.add(static_cast<double>(i));
    const LatencySummary s = t.summary();
    REQUIRE(s.samples == 100);
    REQUIRE(s.last == Catch::Approx(100.0));
    REQUIRE(s.avg == Catch::Approx(50.5));
    REQUIRE(s.max == Catch::Approx(100.0));
    REQUIRE(s.p50 == Catch::Approx(51.0).margin(1.0));
    REQUIRE(s.p99 == Catch::Approx(99.0).margin(1.0));
}

TEST_CASE("PdfDocument writes a structurally valid multi-page PDF") {
    PdfDocument doc;
    doc.text(40, 60, "Hello (world) \\ test", 12.0f, true);
    doc.fillRect(40, 80, 100, 20, PdfColor{0.1f, 0.6f, 0.4f});
    doc.newPage();
    doc.line(40, 40, 200, 40, PdfColor{});
    REQUIRE(doc.pageCount() == 2);

    const std::string pdf = doc.serialize();
    requireWellFormedPdf(pdf);
    REQUIRE(pdf.find("(Hello \\(world\\) \\\\ test) Tj") != std::string::npos);  // parentheses/backslash escaped
    REQUIRE(pdf.find("/Count 2") != std::string::npos);
}

TEST_CASE("The session report builds a valid PDF with every section and each basket's tree") {
    SessionReportInput in;
    in.generatedAt = "2026-09-24 11:00:00";
    in.sessionStartedAt = "2026-09-24 10:00:00";
    in.account = "demo 123";
    in.settings.profitTarget = 1.0;
    in.settings.commissionPerLot = 7.0;
    in.tickDecisionUs.samples = 1000;
    in.tickDecisionUs.avg = 2.5;
    in.ticksProcessed = 1000;
    for (int i = 1; i <= 40; ++i) in.allBaskets.push_back(sampleBasket(i, (i % 3 == 0) ? -0.40 : 0.98));
    in.sessionBaskets.assign(in.allBaskets.end() - 5, in.allBaskets.end());
    in.openPositions = 1;
    in.openNet = 0.35;

    const std::string pdf = buildSessionReportPdf(in);
    requireWellFormedPdf(pdf);
    for (const char* expected :
         {"Overview", "Speed", "Settings", "Basket results", "All baskets", "Position tree per basket", "Basket #40",
          "Basket #1", "Decision per quote"}) {
        INFO(expected);
        REQUIRE(pdf.find(expected) != std::string::npos);
    }
    REQUIRE(std::regex_search(pdf, std::regex("/Count ([3-9]|[1-9][0-9])")));  // spans several pages
}

TEST_CASE("The session report still builds with no baskets at all") {
    SessionReportInput in;
    in.generatedAt = "2026-09-24 11:00:00";
    const std::string pdf = buildSessionReportPdf(in);
    requireWellFormedPdf(pdf);
    REQUIRE(pdf.find("No closed baskets yet.") != std::string::npos);
}

TEST_CASE("Volume statistics: lots, value traded, won/lost lots and P/L per day, week and month") {
    auto basket = [](const std::string& closedAt, double net) {
        BasketRecord b = sampleBasket(1, net);  // legs: BUY 0.01 @ 4282.05 and SELL 0.02 @ 4280.00
        b.closedAt = closedAt;
        return b;
    };
    const std::vector<BasketRecord> baskets = {
        basket("2026-08-31 23:10:00", 1.00),   // Monday 31 Aug
        basket("2026-09-01 09:00:00", 1.20),   // Tuesday 1 Sep, same week
        basket("2026-09-01 15:00:00", -0.50),
        basket("2026-09-07 10:00:00", 0.80),   // next Monday
    };

    const auto days = volumeByPeriod(baskets, VolumePeriod::Day);
    REQUIRE(days.size() == 3);
    REQUIRE(days[0].period == "2026-09-07");  // newest first
    REQUIRE(days[1].period == "2026-09-01");
    REQUIRE(days[1].baskets == 2);
    REQUIRE(days[1].won == 1);
    REQUIRE(days[1].lost == 1);
    REQUIRE(days[1].positions == 4);
    REQUIRE(days[1].lots == Catch::Approx(0.06));
    REQUIRE(days[1].sideLots() == Catch::Approx(0.12));
    REQUIRE(days[1].wonLots == Catch::Approx(0.03));
    REQUIRE(days[1].lostLots == Catch::Approx(0.03));
    REQUIRE(days[1].net == Catch::Approx(0.70));
    REQUIRE(days[1].commission == Catch::Approx(0.42));
    // Value traded: 0.01 x 100 x 4282.05 + 0.02 x 100 x 4280.00 per basket.
    REQUIRE(days[1].notional == Catch::Approx(2 * (4282.05 + 2 * 4280.00)));
    REQUIRE(days[1].netPerLot() == Catch::Approx(0.70 / 0.06));

    const auto weeks = volumeByPeriod(baskets, VolumePeriod::Week);
    REQUIRE(weeks.size() == 2);
    REQUIRE(weeks[0].period == "Week of 2026-09-07");
    REQUIRE(weeks[1].period == "Week of 2026-08-31");
    REQUIRE(weeks[1].baskets == 3);  // Monday 31 Aug belongs with Tuesday 1 Sep

    const auto months = volumeByPeriod(baskets, VolumePeriod::Month);
    REQUIRE(months.size() == 2);
    REQUIRE(months[0].period == "2026-09");
    REQUIRE(months[0].baskets == 3);
    REQUIRE(months[1].period == "2026-08");

    const VolumeBucket total = volumeTotal(baskets);
    REQUIRE(total.baskets == 4);
    REQUIRE(total.lots == Catch::Approx(0.12));
    REQUIRE(total.net == Catch::Approx(2.50));
    REQUIRE(periodKey("garbage", VolumePeriod::Day) == "unknown");

    // Slippage: BUY 0.01 ordered at 4282.00, filled 4282.05 -> $0.05 worse;
    // SELL 0.02 ordered and filled at 4280.00 -> none.
    REQUIRE(legSlippage(baskets[0].legs[0], 100.0) == Catch::Approx(0.05));
    REQUIRE(legSlippage(baskets[0].legs[1], 100.0) == Catch::Approx(0.0));
    REQUIRE(total.slippage == Catch::Approx(4 * 0.05));
}
