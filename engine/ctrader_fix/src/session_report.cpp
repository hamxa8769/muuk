#include "muuk/ctrader_fix/session_report.hpp"

#include "muuk/ctrader_fix/volume_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

#include "muuk/ctrader_fix/pdf_writer.hpp"

namespace muuk::ctrader_fix {

namespace {

constexpr float kLeft = 40.0f;
constexpr float kRight = PdfDocument::kPageWidth - 40.0f;
constexpr float kBottom = PdfDocument::kPageHeight - 50.0f;

const PdfColor kInk{0.10f, 0.12f, 0.18f};
const PdfColor kDim{0.45f, 0.48f, 0.56f};
const PdfColor kRule{0.85f, 0.87f, 0.91f};
const PdfColor kPanel{0.955f, 0.962f, 0.978f};
const PdfColor kNavy{0.07f, 0.09f, 0.17f};
const PdfColor kWhite{1.0f, 1.0f, 1.0f};
const PdfColor kGreen{0.10f, 0.62f, 0.38f};
const PdfColor kRed{0.85f, 0.25f, 0.28f};
const PdfColor kBlue{0.25f, 0.45f, 0.90f};

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%.2f", v < -0.005 ? "-" : (v > 0.005 ? "+" : ""), std::fabs(v));
    return buf;
}

PdfColor pnlColor(double v) {
    if (v > 0.005) return kGreen;
    if (v < -0.005) return kRed;
    return kDim;
}

std::string duration(double seconds) {
    const long s = static_cast<long>(seconds + 0.5);
    char buf[32];
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%ldh %02ldm", s / 3600, (s % 3600) / 60);
    } else if (s >= 60) {
        std::snprintf(buf, sizeof(buf), "%ldm %02lds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%lds", s);
    }
    return buf;
}

struct Stats {
    int baskets = 0;
    int wins = 0;
    int positions = 0;
    double lots = 0.0;
    double gross = 0.0;
    double commission = 0.0;
    double net = 0.0;
    double best = 0.0;
    double worst = 0.0;
    double totalDuration = 0.0;
};

Stats statsOf(const std::vector<BasketRecord>& baskets) {
    Stats s;
    for (const auto& b : baskets) {
        ++s.baskets;
        if (b.net > 0.005) ++s.wins;
        s.positions += static_cast<int>(b.legs.size());
        for (const auto& l : b.legs) s.lots += l.lots;
        s.gross += b.gross;
        s.commission += b.commission;
        s.net += b.net;
        s.best = s.baskets == 1 ? b.net : std::max(s.best, b.net);
        s.worst = s.baskets == 1 ? b.net : std::min(s.worst, b.net);
        s.totalDuration += b.durationSeconds;
    }
    return s;
}

// Keeps track of the writing position and starts a new page (with a small
// running header) whenever the next block wouldn't fit.
class Layout {
public:
    explicit Layout(PdfDocument& doc) : doc_(doc) {}

    PdfDocument& doc() { return doc_; }
    float y() const { return y_; }
    void advance(float dy) { y_ += dy; }

    void ensure(float needed) {
        if (y_ + needed > kBottom) {
            doc_.newPage();
            doc_.text(kLeft, 30.0f, "MUUK report", 8.0f, true, kDim);
            doc_.textRight(kRight, 30.0f, "page " + std::to_string(doc_.pageCount()), 8.0f, false, kDim);
            doc_.line(kLeft, 36.0f, kRight, 36.0f, kRule);
            y_ = 58.0f;
        }
    }

    void section(const std::string& title) {
        ensure(46.0f);
        y_ += 10.0f;
        doc_.text(kLeft, y_, title, 13.0f, true, kInk);
        y_ += 6.0f;
        doc_.line(kLeft, y_, kRight, y_, kRule, 0.8f);
        y_ += 16.0f;
    }

    void setY(float y) { y_ = y; }

private:
    PdfDocument& doc_;
    float y_ = 0.0f;
};

void kpiBox(PdfDocument& doc, float x, float y, float w, const std::string& caption, const std::string& value,
            PdfColor valueColor, const std::string& sub = {}) {
    doc.fillRect(x, y, w, 46.0f, kPanel);
    doc.strokeRect(x, y, w, 46.0f, kRule);
    doc.text(x + 8.0f, y + 13.0f, caption, 7.0f, true, kDim);
    doc.text(x + 8.0f, y + 30.0f, value, 13.0f, true, valueColor);
    if (!sub.empty()) doc.text(x + 8.0f, y + 41.0f, sub, 6.5f, false, kDim);
}

void kpiRow(Layout& lay, const std::vector<std::tuple<std::string, std::string, PdfColor, std::string>>& boxes) {
    lay.ensure(56.0f);
    const float gap = 8.0f;
    const float w = (kRight - kLeft - gap * static_cast<float>(boxes.size() - 1)) / static_cast<float>(boxes.size());
    float x = kLeft;
    for (const auto& [caption, value, color, sub] : boxes) {
        kpiBox(lay.doc(), x, lay.y(), w, caption, value, color, sub);
        x += w + gap;
    }
    lay.advance(56.0f);
}

void overview(Layout& lay, const std::string& title, const Stats& s) {
    lay.ensure(130.0f);
    lay.doc().text(kLeft, lay.y(), title, 9.0f, true, kDim);
    lay.advance(8.0f);
    const double winRate = s.baskets > 0 ? 100.0 * s.wins / s.baskets : 0.0;
    kpiRow(lay, {{"BASKETS CLOSED", std::to_string(s.baskets), kInk,
                  std::to_string(s.wins) + " won / " + std::to_string(s.baskets - s.wins) + " lost"},
                 {"WIN RATE", fmt("%.0f%%", winRate), kInk, ""},
                 {"NET P/L", money(s.net), pnlColor(s.net), "after commission"},
                 {"GROSS P/L", money(s.gross), pnlColor(s.gross), "price moves only"}});
    kpiRow(lay, {{"COMMISSION", money(-s.commission), s.commission > 0.005 ? kRed : kDim, ""},
                 {"AVG BASKET", money(s.baskets ? s.net / s.baskets : 0.0), pnlColor(s.baskets ? s.net / s.baskets : 0.0),
                  s.baskets ? "avg " + duration(s.totalDuration / s.baskets) : ""},
                 {"BEST / WORST", money(s.best) + " / " + money(s.worst), kInk, ""},
                 {"POSITIONS", std::to_string(s.positions), kInk, fmt("%.2f lots traded", s.lots)}});
}

void speedTable(Layout& lay, const SessionReportInput& in) {
    lay.section("Speed");
    PdfDocument& doc = lay.doc();
    const float cols[] = {kLeft, kLeft + 190.0f, kLeft + 250.0f, kLeft + 310.0f, kLeft + 370.0f, kLeft + 430.0f,
                          kRight};
    const char* heads[] = {"Measure", "Last", "Average", "Median", "99th pct", "Max", "Samples"};
    doc.fillRect(kLeft, lay.y() - 11.0f, kRight - kLeft, 16.0f, kPanel);
    doc.text(cols[0] + 4.0f, lay.y(), heads[0], 8.0f, true, kDim);
    for (int i = 1; i < 7; ++i) doc.textRight(i == 6 ? cols[6] - 4.0f : cols[i] + 50.0f, lay.y(), heads[i], 8.0f, true, kDim);
    lay.advance(18.0f);

    auto row = [&](const std::string& name, const LatencySummary& s, const char* unitFmt) {
        doc.text(cols[0] + 4.0f, lay.y(), name, 8.5f, false, kInk);
        if (s.samples == 0) {
            doc.text(cols[1] + 20.0f, lay.y(), "no samples yet", 8.5f, false, kDim);
        } else {
            const double values[] = {s.last, s.avg, s.p50, s.p99, s.max};
            for (int i = 0; i < 5; ++i) doc.textRight(cols[i + 1] + 50.0f, lay.y(), fmt(unitFmt, values[i]), 8.5f, false, kInk);
            doc.textRight(cols[6] - 4.0f, lay.y(), std::to_string(s.samples), 8.5f, false, kInk);
        }
        lay.advance(6.0f);
        doc.line(kLeft, lay.y(), kRight, lay.y(), kRule, 0.4f);
        lay.advance(12.0f);
    };
    row("Decision per quote (microseconds)", in.tickDecisionUs, "%.1f");
    row("Reaction to a fill (microseconds)", in.fillReactionUs, "%.1f");
    row("Broker round trip (milliseconds)", in.brokerRoundTripMs, "%.1f");
    const std::string since = in.statsSince.empty() ? std::string("this session") : "since " + in.statsSince;
    doc.text(kLeft + 4.0f, lay.y(), "Quotes processed " + since + ": " + std::to_string(in.ticksProcessed) +
                                        "   |   Average, max and samples are " + since +
                                        "; median and 99th pct cover the most recent 2048.",
             7.0f, false, kDim);
    lay.advance(10.0f);
    doc.text(kLeft + 4.0f, lay.y(),
             "Decision and reaction are the bot's own processing time; the round trip includes the network and the "
             "broker.",
             7.0f, false, kDim);
    lay.advance(14.0f);
}

// Lots, value traded and results per period.
void volumeTable(Layout& lay, const std::string& title, const std::vector<VolumeBucket>& buckets, std::size_t limit,
                 const VolumeBucket& total) {
    PdfDocument& doc = lay.doc();
    lay.ensure(60.0f);
    doc.text(kLeft, lay.y(), title, 9.0f, true, kDim);
    lay.advance(14.0f);
    // Right edges of the numeric columns.
    const float c[] = {kLeft + 4.0f, kLeft + 125.0f, kLeft + 170.0f, kLeft + 215.0f, kLeft + 290.0f, kLeft + 335.0f,
                       kLeft + 380.0f, kLeft + 432.0f, kLeft + 478.0f, kRight - 4.0f};
    auto header = [&] {
        doc.fillRect(kLeft, lay.y() - 11.0f, kRight - kLeft, 16.0f, kPanel);
        doc.text(c[0], lay.y(), "Period", 7.5f, true, kDim);
        const char* heads[] = {"Baskets", "Won/Lost", "Lots", "Value traded", "Won lots", "Lost lots", "Commission",
                               "Net", "Net/lot"};
        for (int i = 0; i < 9; ++i) doc.textRight(c[i + 1], lay.y(), heads[i], 7.5f, true, kDim);
        lay.advance(16.0f);
    };
    auto row = [&](const VolumeBucket& b, bool bold) {
        if (lay.y() + 14.0f > kBottom) {
            lay.ensure(1000.0f);
            header();
        }
        doc.text(c[0], lay.y(), b.period, 7.5f, bold, kInk);
        doc.textRight(c[1], lay.y(), std::to_string(b.baskets), 7.5f, bold, kInk);
        doc.textRight(c[2], lay.y(), std::to_string(b.won) + " / " + std::to_string(b.lost), 7.5f, bold, kInk);
        doc.textRight(c[3], lay.y(), fmt("%.2f", b.lots), 7.5f, bold, kInk);
        doc.textRight(c[4], lay.y(), fmt("$%.0f", b.notional), 7.5f, bold, kInk);
        doc.textRight(c[5], lay.y(), fmt("%.2f", b.wonLots), 7.5f, bold, kGreen);
        doc.textRight(c[6], lay.y(), fmt("%.2f", b.lostLots), 7.5f, bold, kRed);
        doc.textRight(c[7], lay.y(), money(-b.commission), 7.5f, bold, b.commission > 0.005 ? kRed : kDim);
        doc.textRight(c[8], lay.y(), money(b.net), 7.5f, true, pnlColor(b.net));
        doc.textRight(c[9], lay.y(), money(b.netPerLot()), 7.5f, bold, pnlColor(b.netPerLot()));
        lay.advance(5.0f);
        doc.line(kLeft, lay.y(), kRight, lay.y(), kRule, 0.3f);
        lay.advance(10.0f);
    };
    header();
    if (buckets.empty()) {
        doc.text(c[0], lay.y(), "No closed baskets yet.", 8.0f, false, kDim);
        lay.advance(14.0f);
        return;
    }
    for (std::size_t i = 0; i < buckets.size() && i < limit; ++i) row(buckets[i], false);
    row(total, true);
    lay.advance(4.0f);
}

void settingsTable(Layout& lay, const SessionReportInput& in) {
    lay.section("Settings");
    const LiveSettings& s = in.settings;
    const char* start = s.startSide == muuk::core::StartSide::BuyOnly
                            ? "Buy Stop first"
                            : (s.startSide == muuk::core::StartSide::SellOnly ? "Sell Stop first" : "Both at once");
    const std::vector<std::pair<std::string, std::string>> items = {
        {"First order", start},
        {"Range width", fmt("$%.2f", s.rangeWidth)},
        {"Profit target (net)", s.autoTarget ? "auto: " + std::to_string(s.autoTargetPercent) + "% of the positions' cost"
                                             : fmt("$%.2f", s.profitTarget)},
        {"Stop loss (net)", s.stopLoss > 0 ? fmt("$%.2f", s.stopLoss) : std::string("off")},
        {"Market hours filter", s.market.enabled ? "on, pause " + std::to_string(s.market.pauseMinutesBefore) +
                                                       " min before / " + std::to_string(s.market.pauseMinutesAfter) +
                                                       " min after session ends"
                                                 : std::string("off")},
        {"News guard", s.market.newsGuard
                           ? std::string(s.market.newsHigh ? "High" : "") +
                                 (s.market.newsMedium ? (s.market.newsHigh ? " + Medium" : "Medium") : "") + " " +
                                 s.market.newsCurrencies + ", " + std::to_string(s.market.newsMinutesBefore) +
                                 " min before / " + std::to_string(s.market.newsMinutesAfter) + " after"
                           : std::string("off")},
        {"First lot / increment", fmt("%.2f", s.initialLot) + " / " + fmt("%.2f", s.lotIncrement)},
        {"Entry offset", fmt("$%.2f", s.seedOffset)},
        {"Commission", s.commissionPerMillion > 0 ? fmt("$%.2f per $1M traded, each side", s.commissionPerMillion)
                                                : fmt("$%.2f per lot, round turn", s.commissionPerLot)},
        {"Cooldown between baskets", std::to_string(s.cooldownSeconds) + " s"},
        {"Max orders / max lot", std::to_string(s.maxOrders) + " / " + fmt("%.2f", s.maxLot)},
        {"Max basket loss", s.maxBasketLoss > 0 ? fmt("$%.2f", s.maxBasketLoss) : "off"},
        {"Auto-open next range", s.autoContinue ? "on" : "off"},
    };
    PdfDocument& doc = lay.doc();
    const float colW = (kRight - kLeft) / 2.0f;
    for (std::size_t i = 0; i < items.size(); i += 2) {
        lay.ensure(16.0f);
        for (std::size_t k = 0; k < 2 && i + k < items.size(); ++k) {
            const float x = kLeft + colW * static_cast<float>(k);
            doc.text(x + 4.0f, lay.y(), items[i + k].first, 8.5f, false, kDim);
            doc.textRight(x + colW - 12.0f, lay.y(), items[i + k].second, 8.5f, true, kInk);
        }
        lay.advance(6.0f);
        doc.line(kLeft, lay.y(), kRight, lay.y(), kRule, 0.4f);
        lay.advance(12.0f);
    }
}

// Per-basket net bars, or a cumulative line.
void chart(Layout& lay, const std::string& title, const std::vector<BasketRecord>& baskets, bool cumulative) {
    constexpr float kHeight = 150.0f;
    lay.ensure(kHeight + 40.0f);
    PdfDocument& doc = lay.doc();
    doc.text(kLeft, lay.y(), title, 9.0f, true, kDim);
    lay.advance(8.0f);
    const float top = lay.y();
    const float x0 = kLeft + 44.0f;
    const float x1 = kRight - 6.0f;
    doc.fillRect(kLeft, top, kRight - kLeft, kHeight, kPanel);

    std::vector<double> values;
    double run = 0.0;
    for (const auto& b : baskets) {
        run += b.net;
        values.push_back(cumulative ? run : b.net);
    }
    if (values.empty()) {
        doc.text(kLeft + 12.0f, top + kHeight / 2.0f, "No closed baskets yet.", 9.0f, false, kDim);
        lay.advance(kHeight + 12.0f);
        return;
    }

    double lo = 0.0;
    double hi = 0.0;
    for (const double v : values) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (hi - lo < 0.01) hi = lo + 1.0;
    const double pad = (hi - lo) * 0.08;
    lo -= (lo < 0 ? pad : 0);
    hi += pad;
    const float plotTop = top + 10.0f;
    const float plotBottom = top + kHeight - 16.0f;
    auto yOf = [&](double v) {
        return plotBottom - static_cast<float>((v - lo) / (hi - lo)) * (plotBottom - plotTop);
    };

    // Axis: zero line and the two extremes.
    doc.line(x0, yOf(0.0), x1, yOf(0.0), kDim, 0.6f);
    doc.textRight(x0 - 4.0f, yOf(0.0) + 3.0f, "$0", 7.0f, false, kDim);
    doc.textRight(x0 - 4.0f, plotTop + 3.0f, money(hi), 7.0f, false, kDim);
    if (lo < -0.005) doc.textRight(x0 - 4.0f, plotBottom + 3.0f, money(lo), 7.0f, false, kDim);

    const float slot = (x1 - x0) / static_cast<float>(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float cx = x0 + slot * (static_cast<float>(i) + 0.5f);
        if (cumulative) {
            if (i > 0) {
                const float px = x0 + slot * (static_cast<float>(i) - 0.5f);
                doc.line(px, yOf(values[i - 1]), cx, yOf(values[i]), kBlue, 1.4f);
            }
        } else {
            const float bw = std::max(1.5f, slot * 0.62f);
            const float yv = yOf(values[i]);
            const float y0 = yOf(0.0);
            doc.fillRect(cx - bw / 2.0f, std::min(yv, y0), bw, std::max(0.6f, std::fabs(y0 - yv)), pnlColor(values[i]));
        }
    }
    doc.text(x0, top + kHeight - 4.0f, "basket #" + std::to_string(baskets.front().number) + " (" +
                                            baskets.front().closedAt.substr(0, 10) + ")",
             6.5f, false, kDim);
    doc.textRight(x1, top + kHeight - 4.0f, "basket #" + std::to_string(baskets.back().number) + " (" +
                                                baskets.back().closedAt.substr(0, 10) + ")",
                  6.5f, false, kDim);
    lay.advance(kHeight + 14.0f);
}

void basketTable(Layout& lay, const std::vector<BasketRecord>& newestFirst) {
    lay.section("All baskets");
    PdfDocument& doc = lay.doc();
    const float c[] = {kLeft + 4.0f, kLeft + 34.0f, kLeft + 126.0f, kLeft + 176.0f, kLeft + 206.0f, kLeft + 330.0f,
                       kLeft + 395.0f, kLeft + 455.0f, kRight - 4.0f};
    auto header = [&] {
        doc.fillRect(kLeft, lay.y() - 11.0f, kRight - kLeft, 16.0f, kPanel);
        doc.text(c[0], lay.y(), "#", 8.0f, true, kDim);
        doc.text(c[1], lay.y(), "Closed", 8.0f, true, kDim);
        doc.text(c[2], lay.y(), "Length", 8.0f, true, kDim);
        doc.text(c[3], lay.y(), "Pos.", 8.0f, true, kDim);
        doc.text(c[4], lay.y(), "Ended by", 8.0f, true, kDim);
        doc.textRight(c[6], lay.y(), "Gross", 8.0f, true, kDim);
        doc.textRight(c[7], lay.y(), "Commission", 8.0f, true, kDim);
        doc.textRight(c[8], lay.y(), "Net", 8.0f, true, kDim);
        lay.advance(16.0f);
    };
    header();
    for (const auto& b : newestFirst) {
        if (lay.y() + 14.0f > kBottom) {
            lay.ensure(1000.0f);  // force a page break
            header();
        }
        doc.text(c[0], lay.y(), std::to_string(b.number), 8.0f, false, kInk);
        doc.text(c[1], lay.y(), b.closedAt.substr(0, 16), 8.0f, false, kInk);
        doc.text(c[2], lay.y(), duration(b.durationSeconds), 8.0f, false, kInk);
        doc.text(c[3], lay.y(), std::to_string(b.legs.size()), 8.0f, false, kInk);
        doc.text(c[4], lay.y(), b.closeReason.substr(0, 26), 8.0f, false, kDim);
        doc.textRight(c[6], lay.y(), money(b.gross), 8.0f, false, pnlColor(b.gross));
        doc.textRight(c[7], lay.y(), money(-b.commission), 8.0f, false, b.commission > 0.005 ? kRed : kDim);
        doc.textRight(c[8], lay.y(), money(b.net), 8.0f, true, pnlColor(b.net));
        lay.advance(5.0f);
        doc.line(kLeft, lay.y(), kRight, lay.y(), kRule, 0.3f);
        lay.advance(10.0f);
    }
}

// Each basket as a small tree: the basket on top, one branch per position.
void basketTrees(Layout& lay, const std::vector<BasketRecord>& newestFirst, std::size_t limit) {
    lay.section("Position tree per basket");
    PdfDocument& doc = lay.doc();
    std::size_t shown = 0;
    for (const auto& b : newestFirst) {
        if (shown++ >= limit) break;
        const float rowH = 14.0f;
        const float needed = 34.0f + rowH * static_cast<float>(std::max<std::size_t>(1, b.legs.size()));
        lay.ensure(std::min(needed, 300.0f));

        // Basket node.
        const float top = lay.y();
        doc.fillRect(kLeft, top - 11.0f, kRight - kLeft, 17.0f, kPanel);
        doc.text(kLeft + 6.0f, top, "Basket #" + std::to_string(b.number), 9.0f, true, kInk);
        std::string meta = b.closedAt.substr(0, 16) + "   " + duration(b.durationSeconds);
        if (b.rangeUpper > b.rangeLower) meta += "   range " + fmt("%.2f", b.rangeLower) + " - " + fmt("%.2f", b.rangeUpper);
        meta += "   " + b.closeReason;
        doc.text(kLeft + 72.0f, top, meta, 7.5f, false, kDim);
        doc.textRight(kRight - 6.0f, top, money(b.net), 9.0f, true, pnlColor(b.net));
        lay.advance(16.0f);

        // Branches.
        const float trunkX = kLeft + 14.0f;
        const float trunkTop = lay.y() - 8.0f;
        float lastBranchY = trunkTop;
        for (const auto& l : b.legs) {
            if (lay.y() + rowH > kBottom) {
                doc.line(trunkX, trunkTop, trunkX, lastBranchY, kDim, 0.7f);
                lay.ensure(1000.0f);
                lay.advance(4.0f);
            }
            const float yy = lay.y();
            doc.line(trunkX, yy - 3.0f, trunkX + 12.0f, yy - 3.0f, kDim, 0.7f);
            lastBranchY = yy - 3.0f;
            const PdfColor sideColor = l.buy ? kGreen : kRed;
            doc.text(trunkX + 16.0f, yy, l.buy ? "BUY" : "SELL", 8.0f, true, sideColor);
            std::string detail = fmt("%.2f lot", l.lots);
            if (l.adopted) detail += " (opened earlier)";
            if (!l.note.empty()) detail += " - " + l.note;
            detail += "   req " + fmt("%.2f", l.requestedPrice) + "   fill " + fmt("%.2f", l.entryPrice) + "   exit " +
                      (l.exitPrice > 0 ? fmt("%.2f", l.exitPrice) : std::string("-"));
            if (const double slip = legSlippage(l, b.contractSize); std::fabs(slip) >= 0.005) {
                detail += "   slip " + money(-slip);
            }
            doc.text(trunkX + 44.0f, yy, detail, 7.5f, false, kInk);
            doc.textRight(kRight - 110.0f, yy, "gross " + money(l.gross), 7.5f, false, pnlColor(l.gross));
            doc.textRight(kRight - 56.0f, yy, "comm " + money(-l.commission), 7.5f, false, kDim);
            doc.textRight(kRight - 6.0f, yy, money(l.net), 8.0f, true, pnlColor(l.net));
            lay.advance(rowH);
        }
        if (!b.legs.empty()) {
            if (lastBranchY > trunkTop) doc.line(trunkX, trunkTop, trunkX, lastBranchY, kDim, 0.7f);
        }
        // Basket totals under the branches.
        doc.textRight(kRight - 6.0f, lay.y(), "gross " + money(b.gross) + "   commission " + money(-b.commission) +
                                                  "   net " + money(b.net),
                      7.5f, false, kDim);
        lay.advance(18.0f);
    }
    if (newestFirst.size() > limit) {
        lay.ensure(16.0f);
        doc.text(kLeft, lay.y(), std::to_string(newestFirst.size() - limit) +
                                     " older basket(s) not drawn here -- they're all in the table above.",
                 8.0f, false, kDim);
        lay.advance(16.0f);
    }
}

}  // namespace

std::string buildSessionReportPdf(const SessionReportInput& in) {
    PdfDocument doc;
    Layout lay(doc);

    // Title band.
    doc.fillRect(0.0f, 0.0f, PdfDocument::kPageWidth, 86.0f, kNavy);
    doc.text(kLeft, 42.0f, "MUUK  -  " + in.symbol + " Range Bot Report", 20.0f, true, kWhite);
    std::string sub = "Generated " + in.generatedAt;
    if (!in.account.empty()) sub += "   |   Account " + in.account;
    if (!in.sessionStartedAt.empty()) sub += "   |   Session started " + in.sessionStartedAt;
    if (!in.statsSince.empty()) sub += "   |   Data since " + in.statsSince;
    doc.text(kLeft, 64.0f, sub, 8.5f, false, PdfColor{0.75f, 0.80f, 0.92f});
    lay.setY(108.0f);

    lay.section("Overview");
    overview(lay, "ALL TIME (" + std::to_string(in.allBaskets.size()) + " baskets saved)", statsOf(in.allBaskets));
    lay.advance(4.0f);
    overview(lay, "THIS SESSION", statsOf(in.sessionBaskets));
    if (in.openPositions > 0) {
        lay.ensure(16.0f);
        doc.text(kLeft, lay.y(), "Still open at the time of this report: " + std::to_string(in.openPositions) +
                                     " position(s), live net " + money(in.openNet) + " (not included above).",
                 8.5f, true, pnlColor(in.openNet));
        lay.advance(16.0f);
    }

    speedTable(lay, in);
    settingsTable(lay, in);

    // Charts use the most recent baskets so bars stay readable.
    constexpr std::size_t kChartBaskets = 60;
    std::vector<BasketRecord> recent(in.allBaskets.end() - static_cast<std::ptrdiff_t>(
                                                                std::min(kChartBaskets, in.allBaskets.size())),
                                     in.allBaskets.end());
    lay.section("Basket results");
    chart(lay, "NET P/L PER BASKET (last " + std::to_string(recent.size()) + ")", recent, false);
    chart(lay, "CUMULATIVE NET P/L (last " + std::to_string(recent.size()) + ")", recent, true);

    lay.section("Trading volume");
    lay.ensure(20.0f);
    doc.text(kLeft, lay.y(),
             "Lots: every position counted once (opened and closed). Brokers usually count each side, i.e. twice this. "
             "Value traded = lots x contract size x entry price.",
             7.0f, false, kDim);
    lay.advance(14.0f);
    const VolumeBucket total = volumeTotal(in.allBaskets);
    volumeTable(lay, "BY MONTH", volumeByPeriod(in.allBaskets, VolumePeriod::Month), 24, total);
    volumeTable(lay, "BY DAY (most recent 31)", volumeByPeriod(in.allBaskets, VolumePeriod::Day), 31, total);
    volumeTable(lay, "BY TRADING SESSION (open when the basket started)", volumeBySession(in.allBaskets), 20, total);

    std::vector<BasketRecord> newestFirst(in.allBaskets.rbegin(), in.allBaskets.rend());
    basketTable(lay, newestFirst);
    basketTrees(lay, newestFirst, 150);

    return doc.serialize();
}

}  // namespace muuk::ctrader_fix
