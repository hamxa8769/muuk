#include "dashboard_window.hpp"

#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>

#include "config_discovery.hpp"
#include "glass_ui.hpp"
#include "open_path.hpp"
#include "muuk/core/risk_table.hpp"
#include "muuk/ctrader_fix/basket_history.hpp"

namespace muuk::desktop {

using muuk::core::EngineStatus;
using muuk::core::OrderSide;
using muuk::core::StartSide;
using muuk::ctrader_fix::BasketRecord;
using muuk::ctrader_fix::LatencySummary;
using muuk::ctrader_fix::LiveSettings;
using muuk::ctrader_fix::LiveSnapshot;
using muuk::ctrader_fix::MarketHoursConfig;
using muuk::ctrader_fix::MarketPhase;
using muuk::ctrader_fix::MarketStatus;
using muuk::ctrader_fix::NewsEvent;
using muuk::ctrader_fix::Session;
using muuk::ctrader_fix::VolumeBucket;
using muuk::ctrader_fix::VolumePeriod;
using glass::kAmber;
using glass::kBlue;
using glass::kGreen;
using glass::kGrey;
using glass::kOrange;
using glass::kRed;
using glass::kViolet;

namespace {

ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

ImVec4 pnlColor(double v) {
    if (v > 0.005) return kGreen;
    if (v < -0.005) return kRed;
    return kGrey;
}

// Auto-target levels 20 / 40 / 60 / 80 / 100 = 200 / 240 / 280 / 320 / 360 % of the cost.
int targetLevel(int percent) { return std::clamp((percent - 160) / 2, 20, 100); }
int levelPercent(int level) { return 160 + 2 * level; }

// "2h 05m", "12m 30s", "45s", "3d 4h".
std::string countdown(int64_t ms) {
    if (ms <= 0) return "now";
    const long long s = ms / 1000;
    char buf[32];
    if (s >= 86400) {
        std::snprintf(buf, sizeof(buf), "%lldd %lldh", s / 86400, (s % 86400) / 3600);
    } else if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", s / 3600, (s % 3600) / 60);
    } else if (s >= 60) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%llds", s);
    }
    return buf;
}

// ForexFactory's folder colours.
ImVec4 impactColor(const std::string& impact) {
    if (impact == "High") return kRed;
    if (impact == "Medium") return kOrange;
    if (impact == "Low") return glass::isDark() ? ImVec4(0.95f, 0.85f, 0.30f, 1.0f) : ImVec4(0.62f, 0.52f, 0.00f, 1.0f);
    return kGrey;
}

ImVec4 phaseColor(MarketPhase p) {
    switch (p) {
        case MarketPhase::Open: return kGreen;
        case MarketPhase::SessionPause: return kAmber;
        case MarketPhase::NewsPause: return kRed;
        case MarketPhase::OffHours: return kGrey;
        case MarketPhase::DailyBreak:
        case MarketPhase::Weekend: return kViolet;
    }
    return kGrey;
}

// A small gear icon button (drawn -- the UI font has no gear glyph).
bool gearButton(const char* id, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const ImU32 color = ImGui::GetColorU32(hovered ? glass::palette().text : glass::palette().textDim);
    const float r = size * 0.24f;
    for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
        const ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + d.x * r * 0.9f, c.y + d.y * r * 0.9f), ImVec2(c.x + d.x * r * 1.6f, c.y + d.y * r * 1.6f),
                    color, size * 0.13f);
    }
    dl->AddCircle(c, r, color, 16, size * 0.12f);
    dl->AddCircleFilled(c, r * 0.35f, color, 12);
    return pressed;
}

// A cent account (MT5 "USC"): every amount is in US cents, shown as 12.50¢.
bool g_centUnits = false;

std::string money(double v) {
    char buf[40];
    const char* sign = v < -0.005 ? "-" : (v > 0.005 ? "+" : "");
    if (g_centUnits) {
        std::snprintf(buf, sizeof(buf), "%s%.2f\xC2\xA2", sign, std::fabs(v));
    } else {
        std::snprintf(buf, sizeof(buf), "%s$%.2f", sign, std::fabs(v));
    }
    return buf;
}

std::string price(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

std::string lots(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// "$1,464,993"
std::string dollarsWhole(double v) {
    char digits[32];
    std::snprintf(digits, sizeof(digits), "%.0f", std::fabs(v));
    std::string out;
    const std::string d(digits);
    for (std::size_t i = 0; i < d.size(); ++i) {
        if (i > 0 && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return (v < -0.5 ? "-$" : "$") + out;
}

const char* sideName(OrderSide side) { return side == OrderSide::BuyStop ? "BUY" : "SELL"; }
const char* orderTypeName(OrderSide side) { return side == OrderSide::BuyStop ? "Buy Stop" : "Sell Stop"; }

void withFont(ImFont* font, const auto& draw) {
    if (font) ImGui::PushFont(font);
    draw();
    if (font) ImGui::PopFont();
}

struct CycleState {
    const char* label;
    ImVec4 color;
};

CycleState cycleState(const std::optional<LiveSnapshot>& snap, bool connected) {
    if (!connected || !snap) return {"Disconnected", kGrey};
    if (snap->closeFailed) return {"Close failed - needs attention", kRed};
    if (snap->closingInProgress) return {"Closing basket", kAmber};
    if (snap->cooldownRemainingSeconds > 0) return {"Cooldown", kBlue};
    if (snap->locked) {
        if (snap->escapeTradeOpen) return {"LOCKED - escape trade running", kAmber};
        if (snap->escapeOrdersOut) return {"LOCKED - escape orders out", kAmber};
        return {"LOCKED - loss frozen, waiting for you", kRed};
    }
    if (snap->guardLockPending) return {"MARGIN GUARD - lock stop waiting", kAmber};
    if (snap->marketKnown && !snap->market.canOpenRange &&
        (snap->status == EngineStatus::Idle || snap->status == EngineStatus::Completed)) {
        return {snap->market.label.c_str(), phaseColor(snap->market.phase)};
    }
    switch (snap->status) {
        case EngineStatus::Active:
            return snap->seeding ? CycleState{"Waiting for first fill", kAmber} : CycleState{"Range active", kGreen};
        case EngineStatus::HaltedByRisk:
            return {"Halted by risk limit", kRed};
        case EngineStatus::Completed:
            return snap->settings.autoContinue ? CycleState{"Basket booked - next range soon", kViolet}
                                               : CycleState{"Basket booked", kViolet};
        case EngineStatus::Idle:
        default:
            return {"Idle - press Open range", kGrey};
    }
}

// Why "Open range" is unavailable right now, or nullptr if it's available.
const char* openRangeBlocker(const std::optional<LiveSnapshot>& snap, bool connected) {
    if (!connected || !snap) return "Connect first.";
    if (!snap->bid || !snap->ask) return "Waiting for the first live quote.";
    if (snap->closeFailed)
        return "A position couldn't be closed by the bot - close it in cTrader, then press Sync from broker "
               "(Positions tab).";
    if (snap->status == EngineStatus::Active) return "A range is already running - close it first.";
    if (snap->closingInProgress) return "Previous basket is still closing.";
    if (!snap->positions.empty()) return "Previous range still has open positions.";
    if (!snap->pendingOrders.empty()) return "Previous range still has pending orders.";
    if (snap->marketKnown && !snap->market.canOpenRange) {
        static std::string why;
        why = snap->market.label + " - " + snap->market.detail + " (Settings > Market hours & news)";
        return why.c_str();
    }
    return nullptr;
}

double floatingPnl(const LiveSnapshot& snap) { return snap.live.net; }

// One "label ....... value" row of the P/L breakdown. Costs are shown as
// deductions (with a leading minus) so the column visibly adds up to net.
void breakdownRow(const char* label, const char* help, double value, bool isCost, ImFont* font = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (isCost) {
        ImGui::TextDisabled("  - %s", label);
    } else {
        ImGui::TextUnformatted(label);
    }
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    char buf[32];
    if (isCost) {
        std::snprintf(buf, sizeof(buf), "%s$%.2f", value < -0.005 ? "+" : "-", std::fabs(value));
    } else {
        std::snprintf(buf, sizeof(buf), "%s", money(value).c_str());
    }
    const float w = ImGui::CalcTextSize(buf).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    const ImVec4 color = isCost ? (std::fabs(value) < 0.005 ? glass::palette().textDim : kAmber) : pnlColor(value);
    if (font) ImGui::PushFont(font);
    ImGui::TextColored(color, "%s", buf);
    if (font) ImGui::PopFont();
}

// The range on a fixed layout: the Sell Stop boundary always sits at 28 %
// of the width and the Buy Stop at 72 %, so their labels never move; every
// price is placed relative to them. Rows, top to bottom: side labels,
// pending-order markers, the track (band, boundary lines, filled positions,
// live price), boundary prices, the live price, per-side counts.
float rangeChartHeight() { return ImGui::GetTextLineHeight() * 5.0f + 60.0f; }

void drawRangeChart(const LiveSnapshot& snap, float height) {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glass::Palette& pal = glass::palette();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float smallSize = fs * 0.82f;
    const float textH = ImGui::GetTextLineHeight();
    const float left = origin.x + 8.0f;
    const float right = origin.x + width - 8.0f;
    const float xLo = left + (right - left) * 0.28f;
    const float xHi = left + (right - left) * 0.72f;

    const bool haveRange = snap.rangeUpper > snap.rangeLower;
    const bool haveQuote = snap.bid && snap.ask;
    const double mid = haveQuote ? (*snap.bid + *snap.ask) / 2.0 : 0.0;
    double lo = snap.rangeLower;
    double hi = snap.rangeUpper;
    if (!haveRange) {
        lo = mid - 1.0;
        hi = mid + 1.0;
    }
    const double perPx = (hi - lo) / static_cast<double>(xHi - xLo);
    auto xOf = [&](double p) { return xLo + static_cast<float>((p - lo) / perPx); };
    auto inView = [&](float x) { return std::clamp(x, left + 6.0f, right - 6.0f); };

    const float labelY = origin.y;
    const float markY = labelY + textH + 3.0f;  // pending markers, 9 px tall
    const float trackY = markY + 9.0f + 17.0f;
    const float valueY = trackY + 16.0f;
    const float liveY = valueY + textH + 4.0f;
    const float countY = liveY + textH + 10.0f;

    auto centered = [&](float x, float y, ImU32 color, const std::string& t, float size) {
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x;
        dl->AddText(font, size, ImVec2(std::clamp(x - w * 0.5f, left, right - w), y), color, t.c_str());
    };
    const ImU32 green = ImGui::GetColorU32(kGreen);
    const ImU32 red = ImGui::GetColorU32(kRed);
    const ImU32 text = ImGui::GetColorU32(pal.text);

    // Track and the range band.
    dl->AddRectFilled(ImVec2(left, trackY - 2.0f), ImVec2(right, trackY + 2.0f),
                      ImGui::GetColorU32(glass::isDark() ? ImVec4(1, 1, 1, 0.12f) : ImVec4(0, 0, 0, 0.10f)), 2.0f);
    if (haveRange) {
        dl->AddRectFilledMultiColor(ImVec2(xLo, trackY - 11.0f), ImVec2(xHi, trackY + 11.0f),
                                    ImGui::GetColorU32(withAlpha(kRed, 0.30f)), ImGui::GetColorU32(withAlpha(kGreen, 0.30f)),
                                    ImGui::GetColorU32(withAlpha(kGreen, 0.30f)), ImGui::GetColorU32(withAlpha(kRed, 0.30f)));
        dl->AddLine(ImVec2(xLo, trackY - 15.0f), ImVec2(xLo, trackY + 15.0f), red, 2.5f);
        dl->AddLine(ImVec2(xHi, trackY - 15.0f), ImVec2(xHi, trackY + 15.0f), green, 2.5f);
        centered(xLo, labelY, red, "SELL STOP", fs);
        centered(xHi, labelY, green, "BUY STOP", fs);
        centered(xLo, valueY, text, price(lo), fs);
        centered(xHi, valueY, text, price(hi), fs);
    }

    // Pending orders: a triangle pointing at the track over their price
    // (outlined while held back by the bot).
    for (const auto& o : snap.pendingOrders) {
        const float x = inView(xOf(o.price));
        const ImU32 c = o.side == OrderSide::BuyStop ? green : red;
        const ImVec2 a(x - 6.5f, markY);
        const ImVec2 b(x + 6.5f, markY);
        const ImVec2 t(x, markY + 9.0f);
        if (o.waiting) {
            dl->AddTriangle(a, b, t, c, 1.5f);
        } else {
            dl->AddTriangleFilled(a, b, t, c);
        }
    }

    // Filled positions at their real fill price. One that filled away from
    // its boundary (slippage) gets its own small price label.
    for (const auto& p : snap.positions) {
        const bool buy = p.side == OrderSide::BuyStop;
        const float x = inView(xOf(p.entryPrice));
        const ImVec4 c = buy ? kGreen : kRed;
        dl->AddCircleFilled(ImVec2(x, trackY), 8.0f, ImGui::GetColorU32(withAlpha(c, 0.25f)), 20);
        dl->AddCircleFilled(ImVec2(x, trackY), 4.5f, ImGui::GetColorU32(c), 16);
        const float boundaryX = haveRange ? (buy ? xHi : xLo) : x;
        if (std::fabs(x - boundaryX) >= 14.0f) {
            centered(x, trackY - 13.0f - smallSize, ImGui::GetColorU32(c), price(p.entryPrice), smallSize);
        }
    }

    // Live price: a marker on the track, its price in a tag underneath.
    if (haveQuote) {
        const float raw = xOf(mid);
        const float x = inView(raw);
        dl->AddLine(ImVec2(x, trackY - 13.0f), ImVec2(x, trackY + 13.0f), text, 1.5f);
        dl->AddCircleFilled(ImVec2(x, trackY), 3.5f, text, 12);
        std::string t = price(mid);
        if (raw < left + 6.0f) t = "< " + t;
        if (raw > right - 6.0f) t += " >";
        const float w = ImGui::CalcTextSize(t.c_str()).x + 14.0f;
        const float tx = std::clamp(x - w * 0.5f, left, right - w);
        dl->AddRectFilled(ImVec2(tx, liveY - 2.0f), ImVec2(tx + w, liveY + textH + 2.0f),
                          ImGui::GetColorU32(glass::isDark() ? ImVec4(1, 1, 1, 0.14f) : ImVec4(0, 0, 0, 0.08f)), 7.0f);
        dl->AddTriangleFilled(ImVec2(x - 4.0f, liveY - 2.0f), ImVec2(x + 4.0f, liveY - 2.0f), ImVec2(x, liveY - 6.0f),
                              ImGui::GetColorU32(glass::isDark() ? ImVec4(1, 1, 1, 0.14f) : ImVec4(0, 0, 0, 0.08f)));
        dl->AddText(ImVec2(tx + 7.0f, liveY), text, t.c_str());
    }

    // How far each side has got.
    int buys = 0;
    int sells = 0;
    double buyLots = 0.0;
    double sellLots = 0.0;
    for (const auto& p : snap.positions) {
        if (p.side == OrderSide::BuyStop) {
            ++buys;
            buyLots += p.lots;
        } else {
            ++sells;
            sellLots += p.lots;
        }
    }
    std::string buyPending;
    std::string sellPending;
    for (const auto& o : snap.pendingOrders) {
        (o.side == OrderSide::BuyStop ? buyPending : sellPending) =
            "  |  pending " + lots(o.lots) + (o.waiting ? " (held)" : "");
    }
    const std::string sellLine = "Sell: " + std::to_string(sells) + " filled, " + lots(sellLots) + " lots" + sellPending;
    const std::string buyLine = "Buy: " + std::to_string(buys) + " filled, " + lots(buyLots) + " lots" + buyPending;
    const float sellW = ImGui::CalcTextSize(sellLine.c_str()).x;
    const float buyW = ImGui::CalcTextSize(buyLine.c_str()).x;
    dl->AddText(ImVec2(left, countY), red, sellLine.c_str());
    const float buyY = left + sellW + 16.0f > right - buyW ? countY + textH + 3.0f : countY;
    dl->AddText(ImVec2(right - buyW, buyY), green, buyLine.c_str());
}

// Numeric settings row; marks `dirty` when edited.
void settingDouble(const char* label, const char* help, double* v, double step, const char* fmt, bool* dirty) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    if (ImGui::InputDouble("##v", v, step, step * 10.0, fmt)) *dirty = true;
    ImGui::PopID();
}

void settingCheck(const char* label, const char* help, bool* v, bool* dirty) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::PushID(label);
    if (ImGui::Checkbox("##v", v)) *dirty = true;
    ImGui::PopID();
}

// Margin level in % -> its colour against the two guards.
ImVec4 marginLevelColor(double level, double lockGuard, double stopGuard) {
    if (level < std::max(100.0, stopGuard)) return kRed;
    if (level < std::max(150.0, lockGuard)) return kAmber;
    return kGreen;
}

void settingInt(const char* label, const char* help, int* v, bool* dirty) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    if (ImGui::InputInt("##v", v)) *dirty = true;
    ImGui::PopID();
}

void clampSettings(LiveSettings& s) {
    s.rangeWidth = std::max(0.05, s.rangeWidth);
    s.seedOffset = std::max(0.0, s.seedOffset);
    s.initialLot = std::max(0.01, s.initialLot);
    s.lotIncrement = std::max(0.0, s.lotIncrement);
    s.lotMultiplier = std::clamp(s.lotMultiplier, 1.0, 5.0);
    s.profitTarget = std::max(0.0, s.profitTarget);
    s.stopLoss = std::max(0.0, s.stopLoss);
    s.cooldownSeconds = std::max(0, s.cooldownSeconds);
    s.manualCloseCooldownSeconds = std::max(0, s.manualCloseCooldownSeconds);
    s.maxOrders = std::max(1, s.maxOrders);
    s.maxLot = std::max(0.01, s.maxLot);
    s.maxBasketLoss = std::max(0.0, s.maxBasketLoss);
    s.commissionPerLot = std::max(0.0, s.commissionPerLot);
    s.commissionPerMillion = std::max(0.0, s.commissionPerMillion);
    s.autoTargetPercent = std::clamp(s.autoTargetPercent, 200, 360);  // levels 20..100 (see targetLevel)
    s.recovery.guardLockMarginLevel = std::clamp(s.recovery.guardLockMarginLevel, 0.0, 5000.0);
    s.recovery.guardStopMarginLevel = std::clamp(s.recovery.guardStopMarginLevel, 0.0, 5000.0);
    s.recovery.hedgedMarginPercent = std::clamp(s.recovery.hedgedMarginPercent, 0.0, 100.0);
    s.recovery.escapeMove = std::clamp(s.recovery.escapeMove, 0.1, 100.0);
    s.recovery.escapeBuffer = std::clamp(s.recovery.escapeBuffer, 0.0, 50.0);
    s.recovery.escapeTrailAt = std::clamp(s.recovery.escapeTrailAt, 0.05, 100.0);
    s.recovery.escapeMaxTries = std::clamp(s.recovery.escapeMaxTries, 0, 10);
    s.market.pauseMinutesBefore = std::clamp(s.market.pauseMinutesBefore, 0, 240);
    s.market.pauseMinutesAfter = std::clamp(s.market.pauseMinutesAfter, 0, 240);
    s.market.newsMinutesBefore = std::clamp(s.market.newsMinutesBefore, 0, 240);
    s.market.newsMinutesAfter = std::clamp(s.market.newsMinutesAfter, 0, 240);
    s.accountBalance = std::max(0.0, s.accountBalance);
    s.leverage = std::clamp(s.leverage, 1.0, 3000.0);
    s.escapeFromLevel = std::clamp(s.escapeFromLevel, 0, 30);
    s.escapeProfit = std::max(0.0, s.escapeProfit);
    s.lockAtLoss = std::max(0.0, s.lockAtLoss);
}


// "12.3 us" / "4.1 ms" (with a real micro sign), or "--" before the first sample.
std::string latency(double v, const LatencySummary& s, bool micro) {
    if (s.samples == 0) return "--";
    char buf[32];
    if (micro && v >= 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.2f ms", v / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), micro ? "%.1f \xc2\xb5s" : "%.1f ms", v);
    }
    return buf;
}

// "2026-09-24 14:03:11" -> "14:03:11"
std::string timeOf(const std::string& stamp) {
    const std::size_t space = stamp.find(' ');
    return space == std::string::npos ? stamp : stamp.substr(space + 1);
}

std::string duration(double seconds) {
    char buf[32];
    const long long s = static_cast<long long>(seconds + 0.5);
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", s / 3600, (s % 3600) / 60);
    } else if (s >= 60) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%llds", s);
    }
    return buf;
}

// The trunk-and-branch connector drawn in the first column of a basket's
// leg table, so the legs read as a tree hanging off the basket above them.
void treeConnector(bool buy, bool last) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const float padY = ImGui::GetStyle().CellPadding.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 lineCol = ImGui::GetColorU32(glass::palette().textDim);
    const float x = p.x + 8.0f;
    const float mid = p.y + h * 0.5f;
    dl->AddLine(ImVec2(x, p.y - padY - 1.0f), ImVec2(x, last ? mid : p.y + h + padY + 1.0f), lineCol, 1.5f);
    dl->AddLine(ImVec2(x, mid), ImVec2(x + 11.0f, mid), lineCol, 1.5f);
    dl->AddCircleFilled(ImVec2(x + 16.0f, mid), 4.5f, ImGui::GetColorU32(buy ? kGreen : kRed), 14);
    ImGui::Dummy(ImVec2(26.0f, h));
}

// One basket's legs, in the order they opened.
void basketLegsTable(const BasketRecord& b, bool inProgress) {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("legs", 10, flags)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 46.0f);
    ImGui::TableSetupColumn("Lots");
    ImGui::TableSetupColumn("Stop at");
    ImGui::TableSetupColumn("Filled");
    ImGui::TableSetupColumn("Slippage");
    ImGui::TableSetupColumn("Closed");
    ImGui::TableSetupColumn("Gross");
    ImGui::TableSetupColumn("Net");
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < b.legs.size(); ++i) {
        const auto& leg = b.legs[i];
        const bool open = leg.closedAt.empty();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        treeConnector(leg.buy, i + 1 == b.legs.size());
        ImGui::TableNextColumn();
        ImGui::TextColored(leg.buy ? kGreen : kRed, "%s", leg.buy ? "BUY" : "SELL");
        if (leg.adopted) ImGui::SetItemTooltip("Opened before this session; adopted by a position sync.");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(lots(leg.lots).c_str());
        ImGui::TableNextColumn();
        if (leg.requestedPrice > 0.0) {
            ImGui::TextDisabled("%s", price(leg.requestedPrice).c_str());
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(price(leg.entryPrice).c_str());
        ImGui::TableNextColumn();
        if (const double slip = muuk::ctrader_fix::legSlippage(leg, b.contractSize); std::fabs(slip) < 0.005) {
            ImGui::TextDisabled("$0.00");
        } else {
            ImGui::TextColored(slip > 0 ? kRed : kGreen, "%s", money(-slip).c_str());
            ImGui::SetItemTooltip("Filled %+.2f from the order price", leg.entryPrice - leg.requestedPrice);
        }
        ImGui::TableNextColumn();
        if (open) {
            ImGui::TextColored(inProgress ? kAmber : kRed, "%s", inProgress ? "open" : "not closed");
        } else {
            ImGui::TextUnformatted(price(leg.exitPrice).c_str());
        }
        ImGui::TableNextColumn();
        if (open) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::TextColored(pnlColor(leg.gross), "%s", money(leg.gross).c_str());
        }
        ImGui::TableNextColumn();
        if (open) {
            ImGui::TextDisabled("--");
        } else {
            ImGui::TextColored(pnlColor(leg.net), "%s", money(leg.net).c_str());
            ImGui::SetItemTooltip("Gross %s - commission $%.2f", money(leg.gross).c_str(), leg.commission);
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s%s%s", timeOf(leg.openedAt).c_str(), open ? "" : "  -  ",
                            open ? "" : timeOf(leg.closedAt).c_str());
        if (!leg.positionId.empty()) ImGui::SetItemTooltip("Broker position id: %s", leg.positionId.c_str());
    }
    ImGui::EndTable();
}
}  // namespace

DashboardWindow::DashboardWindow() : prefs_(loadPrefs()) {
    std::error_code ec;
    if (!prefs_.configPath.empty() && std::filesystem::is_regular_file(prefs_.configPath, ec)) {
        setConfigPath(prefs_.configPath);
    } else if (const auto found = findDefaultConfigPath()) {
        setConfigPath(*found);
    }
    darkMode_ = prefs_.darkMode;
    glass::applyTheme(darkMode_);
}

void DashboardWindow::setConfigPath(const std::string& path) {
    const std::size_t n = std::min(path.size(), configPathBuffer_.size() - 1);
    std::copy_n(path.begin(), n, configPathBuffer_.begin());
    configPathBuffer_[n] = '\0';
    settingsLoaded_ = false;
    settingsDirty_ = false;
}

void DashboardWindow::setFonts(ImFont* bold, ImFont* large, ImFont* numbers) {
    boldFont_ = bold;
    largeFont_ = large;
    numberFont_ = numbers ? numbers : large;
}

void DashboardWindow::setThemeChangedCallback(std::function<void(bool dark)> callback) {
    onThemeChanged_ = std::move(callback);
    if (onThemeChanged_) onThemeChanged_(darkMode_);
}

void DashboardWindow::setTheme(bool dark) {
    darkMode_ = dark;
    glass::applyTheme(dark);
    if (onThemeChanged_) onThemeChanged_(dark);
    prefs_.darkMode = dark;
    savePrefs(prefs_);
}

void DashboardWindow::flash(const std::string& message) {
    flashMessage_ = message;
    flashUntil_ = ImGui::GetTime() + 4.0;
}

void DashboardWindow::render(LiveController& controller) {
    glass::drawBackground();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Muuk", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoScrollbar);

    glass::setLowPower(liteMode());
    if (!newsStarted_) {
        controller.startNews(prefsFile().parent_path() / "news_calendar.json");
        newsStarted_ = true;
    }
    controller.setAutoReconnect(prefs_.autoReconnect);
    controller.tick();  // reconnects a dropped connection when it's time
    const bool running = controller.isRunning();
    const std::optional<LiveSnapshot> snap = controller.snapshot();
    samplePerformance(snap);
    const bool connected = running && snap && snap->connected;
    g_centUnits = (snap && snap->brokerAccountFresh && snap->brokerAccount.cent()) || editSettings_.accountKind == "cent";

    // Load the tunable settings from the config file once, so they can be
    // reviewed/edited before connecting; while connected (and not being
    // edited), mirror whatever the running bot actually uses.
    if (!settingsLoaded_ && configPathBuffer_[0] != '\0') {
        if (auto loaded = controller.loadSettings(configPathBuffer_.data())) editSettings_ = *loaded;
        settingsLoaded_ = true;
    }
    if (connected && !settingsDirty_) {
        editSettings_ = snap->settings;
        settingsLoaded_ = true;
    }
    if (accountLoadedFor_ != configPathBuffer_.data()) loadAccountForm(configPathBuffer_.data());

    // Basket history: (re)load it from the file next to the config when the
    // path changes (while connected, start() already loaded it), and copy
    // the controller's list only when it actually changed.
    const std::string configPath(configPathBuffer_.data());
    if (historyPath_ != configPath) {
        std::error_code ec;
        // Only for a real file -- not for every half-typed path.
        if ((!running || controller.demo()) && std::filesystem::is_regular_file(configPath, ec)) {
            controller.attachData(configPath);
            historyPath_ = configPath;
        } else if (running) {
            historyPath_ = configPath;
        }
    }
    if (controller.historyRevision() != historyRevision_) {
        historyRevision_ = controller.historyRevision();
        history_ = controller.history();
    }

    renderHeader(controller, running, connected);
    renderKpiCards(snap, connected);

    const float leftWidth = std::max(380.0f, ImGui::GetContentRegionAvail().x * 0.35f);
    // Scrollable, so nothing in the range panel is ever cut off on a small window.
    ImGui::BeginChild("left", ImVec2(leftWidth, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    renderRangePanel(controller, snap, connected);
    ImGui::EndChild();

    ImGui::SameLine();

    glass::beginCard("right", ImVec2(0, 0));
    renderPages(controller, snap, connected);
    glass::endCard();

    if (openConfirmRequested_) {
        ImGui::OpenPopup("Please confirm");
        openConfirmRequested_ = false;
    }
    renderConfirmPopup(controller);

    if (openExitRequested_) {
        ImGui::OpenPopup("Exit guard");
        openExitRequested_ = false;
    }
    renderExitPopup(controller, snap);

    ImGui::End();
}

void DashboardWindow::renderHeader(LiveController& controller, bool running, bool connected) {
    glass::beginCard("header", ImVec2(0, 0), true);

    // Row 1: brand, connection pill, theme toggle.
    withFont(largeFont_, [] { ImGui::TextColored(kBlue, "MUUK"); });
    ImGui::SameLine(0.0f, 14.0f);
    // Which broker and account: while connected, what the running bot is
    // really logged into; otherwise what the config file will connect to.
    const auto live = controller.snapshot();
    const bool fromBot = running && live && !live->accountLogin.empty();
    const std::string broker = fromBot ? live->brokerUid : std::string(account_.brokerUid.data());
    const std::string login = fromBot ? live->accountLogin : std::string(account_.traderLogin.data());
    const bool isLive = fromBot ? live->environment == "live" : account_.live;
    const std::string symbol = fromBot ? live->symbolName : std::string(account_.symbolName.data());
    const std::string host = fromBot ? live->tradeHost : std::string(account_.tradeHost.data());
    if (fromBot || account_.loaded) {
        glass::pill(isLive ? "LIVE MONEY" : "DEMO", isLive ? kRed : kBlue);
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        withFont(boldFont_, [&] {
            ImGui::Text("%s  #%s", broker.empty() ? "?" : broker.c_str(), login.empty() ? "?" : login.c_str());
        });
        ImGui::SetItemTooltip("%s account %s at %s\nServer: %s\nSymbol: %s\n%s", isLive ? "LIVE" : "Demo", login.c_str(),
                              broker.c_str(), host.c_str(), symbol.c_str(),
                              fromBot ? "This is the account the running bot is logged into."
                                      : "Not connected: this is the account Connect will use (Settings > Account).");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled("%s  |  %s", symbol.c_str(), host.c_str());
        ImGui::SameLine(0.0f, 14.0f);
    }
    // Audit: the bot's own checks, worst first.
    if (connected && live && !live->audit.empty()) {
        int worst = 0;
        int flagged = 0;
        std::string list;
        for (const auto& c : live->audit) {
            worst = std::max(worst, c.level);
            if (c.level > 0) {
                ++flagged;
                list += c.name + ": " + c.detail + "\n";
            }
        }
        const std::string text = worst == 0 ? "Audit OK" : (worst >= 2 ? "Audit: needs you" : "Audit: " + std::to_string(flagged) + " warning" + (flagged > 1 ? "s" : ""));
        glass::pill(text.c_str(), worst == 0 ? kGreen : (worst >= 2 ? kRed : kAmber), worst >= 2);
        if (ImGui::IsItemClicked()) selectTab_ = Tab::Audit;
        ImGui::SetItemTooltip("%s", worst == 0 ? "Every self-check passed (checked every second). Click for details."
                                               : (list + "Click for details.").c_str());
        ImGui::SameLine(0.0f, 10.0f);
    }
    // Broker check: the bot's positions/orders compared with the broker's own lists.
    if (connected && live && !live->brokerCheckedAt.empty()) {
        if (live->brokerIssuesRecent > 0) {
            glass::pill("Broker check: issues", kRed);
            std::string all;
            for (const auto& issue : live->brokerIssues) all += issue + "\n";
            ImGui::SetItemTooltip("Found in the last 5 minutes (details in the Log):\n%s", all.c_str());
        } else {
            glass::pill(("Broker verified " + live->brokerCheckedAt.substr(11)).c_str(), kGreen);
            ImGui::SetItemTooltip("Every position and pending order shown here matched the broker's own lists at %s. "
                                  "Checked every 30 s.",
                                  live->brokerCheckedAt.c_str());
        }
        ImGui::SameLine(0.0f, 10.0f);
    }
    if (const auto bridge = controller.bridgeStatus()) {
        if (bridge->ready) {
            const std::string text = "MT5 " + bridge->login + (bridge->currency == "USC" ? "  cent" : "");
            glass::pill(text.c_str(), bridge->hedging ? kGreen : kRed);
            ImGui::SetItemTooltip("MetaTrader 5 through the Muuk bridge\n%s -- %s\nAccount %s, %s, 1:%.0f%s\nSymbol %s, min lot "
                                  "%.2f\nBalance %.2f  Equity %.2f  Margin %.2f",
                                  bridge->company.c_str(), bridge->server.c_str(), bridge->login.c_str(),
                                  bridge->currency.c_str(), bridge->leverage, bridge->hedging ? ", hedging" : ", NETTING (not usable)",
                                  bridge->symbol.c_str(), bridge->volumeMin, bridge->balance, bridge->equity, bridge->margin);
        } else {
            glass::pill("MT5: EA not connected", kRed, true);
            ImGui::SetItemTooltip("Open MetaTrader 5, attach MuukBridge to the gold chart and allow algo trading "
                                  "(see docs/mt5.md). The bridge waits on port %d.",
                                  bridgeEaPortHint_);
        }
        ImGui::SameLine(0.0f, 10.0f);
    }
    if (connected) {
        glass::pill("Live", kGreen, true);
    } else if (running) {
        glass::pill("Connecting", kAmber, true);
    } else {
        glass::pill("Offline", kGrey);
    }

    const float themeWidth = 92.0f;
    const float autoWidth = ImGui::CalcTextSize("Connect when the app opens").x + ImGui::GetFrameHeight() + 24.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - themeWidth - autoWidth - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Checkbox("Connect when the app opens", &prefs_.autoConnect)) savePrefs(prefs_);
    ImGui::SetItemTooltip("Next time the app starts it connects by itself with the same config and picks up "
                          "the running basket -- for an unattended PC/VPS.");
    ImGui::SameLine(ImGui::GetWindowWidth() - themeWidth - ImGui::GetStyle().WindowPadding.x);
    if (glass::button(darkMode_ ? "Light mode" : "Dark mode", kViolet, ImVec2(themeWidth, 0))) {
        setTheme(!darkMode_);
    }

    // Row 2: config file + connect/disconnect.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Config file");
    ImGui::SameLine();
    const float buttonsWidth = 2 * 120.0f + ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - buttonsWidth));
    ImGui::BeginDisabled(running);
    if (ImGui::InputText("##configpath", configPathBuffer_.data(), configPathBuffer_.size())) {
        settingsLoaded_ = false;  // reload tunables from the newly typed file
        settingsDirty_ = false;
    }
    ImGui::SameLine();
    if (glass::button("Connect", kGreen, ImVec2(120, 0), glass::ButtonKind::Primary)) {
        settingsDirty_ = false;
        controller.start(std::string(configPathBuffer_.data()));
        prefs_.configPath = configPathBuffer_.data();  // open with this config next time
        savePrefs(prefs_);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (glass::button("Disconnect", glass::palette().neutral, ImVec2(120, 0))) controller.stop();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Disconnects from the broker. Open positions and pending orders stay at the broker as they are.");

    const std::string err = controller.lastError();
    if (!err.empty()) {
        ImGui::TextColored(kRed, "Error: %s", err.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) controller.clearError();
    }
    if (!flashMessage_.empty() && ImGui::GetTime() < flashUntil_) {
        ImGui::TextColored(kAmber, "%s", flashMessage_.c_str());
    }
    // What happened to the saved basket when the bot (re)connected.
    if (const auto snap = controller.snapshot(); snap && !snap->resumeNote.empty() &&
                                                 snap->resumeNote != dismissedResumeNote_) {
        const bool resumed = snap->resumeNote.rfind("Resumed", 0) == 0;
        ImGui::TextColored(resumed ? kGreen : kAmber, "%s", snap->resumeNote.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("OK")) dismissedResumeNote_ = snap->resumeNote;
    }
    renderMarketStrip(controller, controller.snapshot(), connected);
    glass::endCard();
}

void DashboardWindow::renderPages(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                  bool connected) {
    if (selectTab_ != Tab::None) {
        currentTab_ = selectTab_;
        selectTab_ = Tab::None;
    }
    static constexpr Tab kOrder[] = {Tab::Positions, Tab::Orders, Tab::History,     Tab::Risk,     Tab::News,
                                     Tab::Audit,     Tab::Volume, Tab::Performance, Tab::Settings, Tab::Log};
    constexpr int kPages = static_cast<int>(sizeof(kOrder) / sizeof(kOrder[0]));
    // Ctrl+1..9, Ctrl+0 jump straight to a page (not while typing into a field).
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput) {
        for (int i = 0; i < kPages; ++i) {
            const ImGuiKey key = static_cast<ImGuiKey>(i < 9 ? ImGuiKey_1 + i : ImGuiKey_0);
            if (ImGui::IsKeyPressed(key, false)) currentTab_ = kOrder[i];
        }
    }

    const MarketStatus market = currentMarket(controller, snap, connected);
    int worst = 0;
    if (snap) {
        for (const auto& c : snap->audit) worst = std::max(worst, c.level);
    }
    struct Item {
        Tab tab;
        const char* label;
        std::string badge;
        ImVec4 color;
    };
    const Item items[] = {
        {Tab::Positions, "Positions", std::to_string(snap ? snap->positions.size() : 0), kBlue},
        {Tab::Orders, "Pending orders", std::to_string(snap ? snap->pendingOrders.size() : 0), kViolet},
        {Tab::History, "History", std::to_string(history_.size()), kGrey},
        {Tab::Risk, "Risk & guards",
         !snap || snap->marginLevelNow < 0.0 ? std::string()
         : snap->locked                      ? std::string("locked")
         : snap->guardLockPending            ? std::string("guard")
                                             : std::to_string(static_cast<int>(std::min(snap->marginLevelNow, 99999.0))) + "%",
         !snap || snap->marginLevelNow < 0.0 ? kGrey
         : snap->locked                      ? kRed
         : snap->guardLockPending            ? kAmber
                                             : marginLevelColor(snap->marginLevelNow,
                                                                snap->settings.recovery.guardLockMarginLevel,
                                                                snap->settings.recovery.guardStopMarginLevel)},
        {Tab::News, "News", market.phase == MarketPhase::NewsPause ? "guard" : "", kRed},
        {Tab::Audit, "Audit", !snap ? "" : worst >= 2 ? "!" : worst == 1 ? "check" : "ok",
         worst >= 2 ? kRed : worst == 1 ? kAmber : kGreen},
        {Tab::Volume, "Volume", "", kGrey},
        {Tab::Performance, "Performance", "", kGrey},
        {Tab::Settings, "Settings", settingsDirty_ ? "unsaved" : "", kAmber},
        {Tab::Log, "Log", "", kGrey},
    };

    // The menu: one icon on the left. Hovering it slides the whole menu out
    // over the page; picking a page closes it again. The page always gets
    // all the width next to the icon.
    const float railWidth = 54.0f;
    const float iconSize = 40.0f;
    const ImVec2 railPos = ImGui::GetCursorScreenPos();
    const float railHeight = ImGui::GetContentRegionAvail().y;
    const bool alert = worst >= 2 || (snap && (snap->locked || snap->guardLockPending));
    const float openness = glass::ease("menu_drawer", menuOpen_ ? 1.0f : 0.0f, 14.0f);
    ImGui::BeginChild("rail", ImVec2(railWidth, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2((railWidth - iconSize) * 0.5f, 4.0f));
    glass::menuIcon("##menu_icon", iconSize, openness, alert);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    switch (currentTab_) {
        case Tab::None:
        case Tab::Positions:
            if (snap) {
                renderPositionsTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            break;
        case Tab::Orders:
            if (snap) {
                renderOrdersTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            break;
        case Tab::History:
            renderHistoryTab(controller, snap);
            break;
        case Tab::Risk:
            renderRiskPage(controller, snap, connected);
            break;
        case Tab::News:
            renderNewsTab(controller, snap, connected);
            break;
        case Tab::Audit:
            renderAuditTab(snap, connected);
            break;
        case Tab::Volume:
            ImGui::BeginChild("volume_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            renderVolumeTab();
            ImGui::EndChild();
            break;
        case Tab::Performance:
            ImGui::BeginChild("performance_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            renderPerformanceTab(controller, snap);
            ImGui::EndChild();
            break;
        case Tab::Settings:
            renderSettingsPanel(controller, snap, connected);
            break;
        case Tab::Log:
            renderLogTab(controller);
            break;
    }
    ImGui::EndChild();

    // The drawer.
    const float drawerWidth = 236.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto inside = [&](ImVec2 a, ImVec2 b) { return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y; };
    const float width = railWidth + (drawerWidth - railWidth) * openness;
    const bool overIcon = inside(railPos, ImVec2(railPos.x + railWidth, railPos.y + iconSize + 12.0f));
    const bool overDrawer = openness > 0.05f && inside(railPos, ImVec2(railPos.x + width, railPos.y + railHeight));
    if (menuSuppressed_ && !overIcon && !overDrawer) menuSuppressed_ = false;  // left it after a pick
    menuOpen_ = !menuSuppressed_ && (overIcon || (menuOpen_ && overDrawer));
    if (openness < 0.01f) return;

    ImGui::SetNextWindowPos(railPos);
    ImGui::SetNextWindowSize(ImVec2(width, railHeight));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##menu_drawer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = railPos;
        const ImVec2 b(railPos.x + width, railPos.y + railHeight);
        const float r = 14.0f;
        if (!glass::lowPower()) {
            for (int i = 1; i <= 6; ++i) {
                const float g = static_cast<float>(i) * 2.0f;
                dl->AddRectFilled(ImVec2(a.x, a.y + g * 0.5f), ImVec2(b.x + g, b.y + g * 0.5f),
                                  ImGui::GetColorU32(withAlpha(glass::palette().shadow, 0.05f * openness)), r + g);
            }
        }
        ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        fill.w = 0.97f * std::min(1.0f, openness * 1.6f);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(fill), r);
        dl->AddRect(a, b, ImGui::GetColorU32(withAlpha(kBlue, 0.30f * openness)), r, 0, 1.0f);
    }
    ImGui::SetCursorPos(ImVec2((railWidth - iconSize) * 0.5f, 4.0f));
    glass::menuIcon("##menu_icon_open", iconSize, openness, alert);
    if (openness > 0.35f) {
        ImGui::SameLine(railWidth + 4.0f);
        ImGui::SetCursorPosY(4.0f + (iconSize - ImGui::GetTextLineHeight()) * 0.5f);
        withFont(boldFont_, [&] { ImGui::TextColored(withAlpha(glass::palette().text, openness), "Menu"); });
    }
    ImGui::SetCursorPos(ImVec2(10.0f, iconSize + 16.0f));
    if (openness > 0.25f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp((openness - 0.25f) / 0.6f, 0.0f, 1.0f));
        ImGui::BeginGroup();
        ImGui::PushItemWidth(width - 20.0f);
        for (int i = 0; i < kPages; ++i) {
            const Item& it = items[i];
            const std::string id = std::string(it.label) + "##rail";
            ImGui::SetCursorPosX(10.0f);
            ImGui::BeginChild(("row" + std::to_string(i)).c_str(), ImVec2(width - 20.0f, ImGui::GetFrameHeight() + 8.0f),
                              ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            if (glass::railItem(id.c_str(), it.badge.c_str(), it.color, currentTab_ == it.tab, kBlue)) {
                currentTab_ = it.tab;
                menuOpen_ = false;
                menuSuppressed_ = true;  // closes now; opens again once the mouse has left and comes back
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+%d", (i + 1) % 10);
            ImGui::EndChild();
        }
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

void DashboardWindow::renderKpiCards(const std::optional<LiveSnapshot>& snap, bool connected) {
    constexpr int kCount = 7;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * (kCount - 1)) / kCount;
    const float numberH = numberFont_ ? numberFont_->FontSize : ImGui::GetTextLineHeight();
    const float height = ImGui::GetTextLineHeight() * 2.0f + numberH + ImGui::GetStyle().WindowPadding.y * 2 + 10.0f;

    // `target`: the tab a click opens (Tab::None = not clickable).
    auto card = [&](const char* id, const char* caption, const std::string& value, ImVec4 valueColor,
                    const std::string& sub, Tab target, const char* targetName, bool last = false) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        glass::beginCard(id, ImVec2(width, height), false, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("%s", caption);
        withFont(numberFont_, [&] { ImGui::TextColored(valueColor, "%s", value.c_str()); });
        if (!sub.empty()) ImGui::TextDisabled("%s", sub.c_str());
        glass::endCard();
        const ImVec2 max(min.x + width, min.y + height);
        if (target != Tab::None && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(min, max)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(withAlpha(kBlue, 0.75f)),
                                                ImGui::GetStyle().ChildRounding, 0, 1.5f);
            ImGui::SetTooltip("Open %s", targetName);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selectTab_ = target;
        }
        if (!last) ImGui::SameLine();
    };

    const ImVec4 text = glass::palette().text;
    const bool haveQuote = snap && snap->bid && snap->ask;
    double buyLots = 0.0;
    double sellLots = 0.0;
    if (snap) {
        for (const auto& p : snap->positions) (p.side == OrderSide::BuyStop ? buyLots : sellLots) += p.lots;
    }
    const double floating = connected && snap ? floatingPnl(*snap) : 0.0;
    const double shownFloating = glass::ease("kpi_floating", static_cast<float>(floating), 12.0f);
    // Booked results come from the saved history, so they survive restarts.
    double allTimeNet = 0.0;
    double todayNet = 0.0;
    const std::string today = muuk::ctrader_fix::localTimestamp().substr(0, 10);
    for (const auto& b : history_) {
        allTimeNet += b.net;
        if (b.closedAt.rfind(today, 0) == 0) {
            todayNet += b.net;
        }
    }
    const double shownBooked = glass::ease("kpi_booked", static_cast<float>(allTimeNet), 8.0f);
    refreshVolume();
    auto bucket = [](const std::vector<VolumeBucket>& rows, const std::string& key) {
        for (const auto& r : rows) {
            if (r.period == key) return r;
        }
        return VolumeBucket{};
    };
    const VolumeBucket volToday = bucket(volumeRows_[0], nowDay_);
    const VolumeBucket volMonth = bucket(volumeRows_[2], nowMonth_);

    card("k_price", "BID / ASK", haveQuote ? price(*snap->bid) + " / " + price(*snap->ask) : std::string("--"), text,
         snap ? snap->symbolName : "", Tab::None, "");
    card("k_spread", "SPREAD", haveQuote ? price(*snap->ask - *snap->bid) : std::string("--"), text,
         snap ? "Range " + price(snap->rangeLower) + " - " + price(snap->rangeUpper) : "", Tab::Settings,
         "Settings (range width, entry offset)");
    card("k_float", "NET P/L (AFTER COSTS)", connected ? money(shownFloating) : std::string("--"), pnlColor(floating),
         connected && snap ? "Gross " + money(snap->live.gross) + " | costs $" + price(snap->live.costs())
                           : std::string(""),
         Tab::Positions, "Positions");
    card("k_pos", "OPEN POSITIONS", snap ? std::to_string(snap->positions.size()) : std::string("--"), text,
         snap ? "Buy " + lots(buyLots) + " / Sell " + lots(sellLots) : "", Tab::Positions, "Positions");
    card("k_ord", "PENDING ORDERS", snap ? std::to_string(snap->pendingOrders.size()) : std::string("--"), text,
         cycleState(snap, connected).label, Tab::Orders, "Pending orders");
    card("k_real", "BOOKED NET (ALL TIME)", money(shownBooked), pnlColor(allTimeNet),
         std::to_string(history_.size()) + " baskets | today " + money(todayNet), Tab::History, "History");
    card("k_vol", "VOLUME TODAY", lots(volToday.lots) + " lots", text,
         "month " + lots(volMonth.lots) + " | all " + lots(volumeTotal_.lots), Tab::Volume, "Volume", true);
}

void DashboardWindow::renderRangePanel(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                       bool connected) {
    glass::beginCard("range_panel", ImVec2(0, 0), true);

    withFont(boldFont_, [] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Range");
    });
    ImGui::SameLine();
    const CycleState state = cycleState(snap, connected);
    const bool busy = connected && snap && (snap->status == EngineStatus::Active || snap->closingInProgress);
    glass::pill(state.label, state.color, busy);
    if (snap && snap->cooldownRemainingSeconds > 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kBlue, "%ds", snap->cooldownRemainingSeconds);
    }

    if (snap && connected && snap->marginLevelNow >= 0.0) {
        ImGui::SameLine();
        const auto& rc = snap->settings.recovery;
        char text[96];
        std::snprintf(text, sizeof(text), "Margin level %.0f%%", std::min(snap->marginLevelNow, 99999.0));
        glass::pill(text, marginLevelColor(snap->marginLevelNow, rc.guardLockMarginLevel, rc.guardStopMarginLevel));
        ImGui::SetItemTooltip("Equity %s / margin %s (worked out from the balance in Settings -- FIX doesn't send "
                              "them). Guards: next level held back under %.0f%%, lock at once under %.0f%%.",
                              money(snap->equityNow).c_str(), money(snap->marginNow).c_str(),
                              rc.guardLockMarginLevel, rc.guardStopMarginLevel);
    }
    if (snap && snap->guardLockPending) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("Margin guard: the next level was not placed (it would have left the margin level under "
                           "%.0f%%). A lock stop %.2f lot waits at %.2f instead. If the price turns back first, the "
                           "basket closes at %s.",
                           snap->settings.recovery.guardLockMarginLevel, snap->guardLockLots, snap->guardLockPrice,
                           money(std::max(0.01, snap->settings.escapeProfit)).c_str());
        ImGui::PopStyleColor();
    }
    if (snap && snap->locked) {
        ImGui::PushStyleColor(ImGuiCol_Text, snap->escapeTradeOpen || snap->escapeOrdersOut ? kAmber : kRed);
        std::string how = "Press Close range when you decide -- it closes both sides.";
        if (snap->settings.recovery.escapeEnabled) {
            how = snap->escapeTradeOpen
                      ? std::string("Escape trade running") +
                            (snap->escapeTrailing ? " (it locks again if the price comes back to its entry)" : "") +
                            ": the basket closes at break-even, or locks again if the price turns back."
                  : snap->escapeOrdersOut ? std::string("Escape orders wait outside the range for a breakout.")
                  : snap->escapeTries >= snap->settings.recovery.escapeMaxTries
                      ? std::string("Escape tries used up. Press Close range when you decide.")
                      : std::string("Escape waits for enough margin (see the Audit).");
        }
        ImGui::TextWrapped("Locked since %s at %s: the pending orders were cancelled and a hedge froze the loss. "
                           "It stays about the same wherever the price goes (swap is charged every night). %s",
                           snap->lockedAt.size() > 11 ? snap->lockedAt.substr(11).c_str() : snap->lockedAt.c_str(),
                           money(snap->lockedLoss).c_str(), how.c_str());
        ImGui::PopStyleColor();
    }
    const float chartHeight = rangeChartHeight();
    if (snap && connected) {
        drawRangeChart(*snap, chartHeight);
    } else {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(w, chartHeight));
        const char* msg = "Connect to see the live range";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (chartHeight - ts.y) / 2),
                                            ImGui::GetColorU32(glass::palette().textDim), msg);
    }
    // Net P/L vs target (and its gear) right under the chart, so it's always in view.
    if (snap && connected) renderTargetBar(controller, *snap, connected);

    if (snap && connected) {
        ImGui::Spacing();
        withFont(boldFont_, [] { ImGui::TextUnformatted("Costs & net P/L"); });
        ImGui::SameLine();
        ImGui::TextDisabled("(all open positions, if closed now)");
        if (ImGui::BeginTable("pnl_breakdown", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            const auto& b = snap->live;
            breakdownRow("Gross P/L", "Price move from each order's requested price to the current mid.", b.gross,
                         false);
            breakdownRow("Slippage", "Fills worse (+) or better (-) than the requested Stop price.", b.slippage, true);
            breakdownRow("Spread", "Cost of closing at bid/ask instead of mid, right now.", b.spread, true);
            breakdownRow("Commission",
                         "Open + close, worked out per position exactly as the broker does (Settings > Commission). "
                         "Same figure as cTrader's Commission column.",
                         b.commission, true);
            breakdownRow("Swap", "Overnight financing - not tracked yet.", b.swap, true);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Separator();
            ImGui::TableNextColumn();
            ImGui::Separator();
            breakdownRow("Net P/L", "What you would actually take home if everything closed now.", b.net, false,
                         boldFont_);
            ImGui::EndTable();
        }
        if (snap->settings.commissionPerLot <= 0.0 && snap->settings.commissionPerMillion <= 0.0) {
            ImGui::TextColored(kAmber, "Commission is set to $0 - enter your broker's rate in Strategy settings.");
        }
        // The same numbers in cTrader's own columns, to compare side by side.
        const auto& b = snap->live;
        ImGui::TextDisabled("As cTrader shows it:  Gross %s   Commission %s   Net %s",
                            money(b.net + b.commission + b.swap).c_str(), money(-b.commission).c_str(),
                            money(b.net).c_str());
        ImGui::SetItemTooltip("cTrader's Gross is the move from each actual fill to the price it would close at now "
                              "(bid for buys, ask for sells) -- slippage and spread are already inside it.");
    }


    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    const float bigHeight = ImGui::GetFrameHeight() * 1.7f;

    // Text for the Close range confirmation, from the live settings.
    if (snap && snap->settings.autoContinue && snap->settings.manualCloseCooldownSeconds > 0) {
        manualReopenText_ = "All pending orders are cancelled and every open position is closed at market.\n"
                            "Once everything is closed, a new range opens after " +
                            std::to_string(snap->settings.manualCloseCooldownSeconds) +
                            " s (Auto-open is on).";
    } else {
        manualReopenText_ = "All pending orders are cancelled and every open position is closed at market.\n"
                            "The bot then waits until you press Open range.";
    }
    const char* blocker = openRangeBlocker(snap, connected);
    ImGui::BeginDisabled(blocker != nullptr);
    if (glass::button("Open range", kGreen, ImVec2(half, bigHeight), glass::ButtonKind::Primary)) {
        if (!controller.openRange()) flash("Could not open a range - see the Log tab for why.");
    }
    ImGui::EndDisabled();
    if (blocker) {
        ImGui::SetItemTooltip("%s", blocker);
    } else {
        ImGui::SetItemTooltip("Opens a new range at the current price.");
    }

    ImGui::SameLine();
    const bool somethingToClose =
        connected && snap &&
        (snap->status == EngineStatus::Active || !snap->positions.empty() || !snap->pendingOrders.empty());
    ImGui::BeginDisabled(!somethingToClose);
    if (glass::button("Close range", kRed, ImVec2(half, bigHeight), glass::ButtonKind::Primary)) {
        confirm_ = Confirm::CloseRange;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Cancels all pending orders and closes all open positions. With Auto-open on, a new range "
                          "opens after the 'Cooldown after Close range' (Settings); otherwise it waits for Open range.");

    ImGui::BeginDisabled(!connected || !snap);
    bool autoContinue = snap ? snap->settings.autoContinue : editSettings_.autoContinue;
    if (ImGui::Checkbox("Auto-open the next range after each basket closes", &autoContinue) && snap) {
        LiveSettings s = snap->settings;  // only this field -- other edits wait for Save & apply
        s.autoContinue = autoContinue;
        controller.applySettings(s);
        if (configPathBuffer_[0] != '\0') (void)controller.saveSettings(configPathBuffer_.data(), s);
        editSettings_.autoContinue = autoContinue;
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!connected || !snap || snap->positions.empty());
    if (glass::button("Close all positions", kRed, ImVec2(half, 0))) {
        confirm_ = Confirm::CloseAllPositions;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected || !snap || snap->pendingOrders.empty());
    if (glass::button("Cancel all pending", kAmber, ImVec2(half, 0))) {
        confirm_ = Confirm::CancelAllPending;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();

    if (snap && connected) renderSpeed(*snap);

    glass::endCard();
}

void DashboardWindow::renderSettingsPanel(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                          bool connected) {
    ImGui::BeginChild("settings_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    renderAccountSection(controller.isRunning());
    ImGui::Spacing();
    withFont(boldFont_, [] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Strategy settings");
    });
    if (settingsDirty_) {
        ImGui::SameLine();
        glass::pill("edited", kAmber);
    }
    ImGui::TextDisabled("Saved to your config file, so the app always opens with them. Range and lots apply from "
                        "the next range; profit target, stop loss and market hours at once.");

    ImGui::BeginDisabled(!settingsLoaded_);
    if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        LiveSettings& s = editSettings_;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("First order");
        ImGui::SetItemTooltip("How each range starts. 'Buy Stop first': only a Buy Stop is placed; the Sell Stop "
                              "follows once it fills, and so on -- one pending order at a time.");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* startItems[] = {"Buy Stop first", "Sell Stop first", "Both at once"};
        int startIndex = s.startSide == StartSide::BuyOnly ? 0 : (s.startSide == StartSide::SellOnly ? 1 : 2);
        if (ImGui::Combo("##startside", &startIndex, startItems, 3)) {
            s.startSide = startIndex == 0 ? StartSide::BuyOnly : (startIndex == 1 ? StartSide::SellOnly : StartSide::Both);
            settingsDirty_ = true;
        }

        settingDouble("Range width ($)", "Distance between the Buy Stop (top) and Sell Stop (bottom) of a range.",
                      &s.rangeWidth, 0.10, "%.2f", &settingsDirty_);
        ImGui::BeginDisabled(s.autoTarget);
        settingDouble("Profit target ($)", "Floating net profit at which the whole basket is booked and closed.",
                      &s.profitTarget, 0.25, "%.2f", &settingsDirty_);
        ImGui::EndDisabled();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Auto profit target");
        ImGui::SetItemTooltip("On: the target is this %% of what the open positions cost (commission open + close + "
                              "spread) -- small for one 0.01 lot, bigger with every fill. Replaces the $ target.");
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("On##autotarget", &s.autoTarget)) settingsDirty_ = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.autoTarget);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* levelItems[] = {"20  =  200% of cost", "40  =  240% of cost", "60  =  280% of cost",
                                    "80  =  320% of cost", "100  =  360% of cost"};
        int levelIndex = std::clamp(targetLevel(s.autoTargetPercent) / 20 - 1, 0, 4);
        if (ImGui::Combo("##autopct", &levelIndex, levelItems, 5)) {
            s.autoTargetPercent = levelPercent((levelIndex + 1) * 20);
            settingsDirty_ = true;
        }
        ImGui::EndDisabled();
        settingDouble("Stop loss ($)",
                      "Hard loss cap: when the basket's net P/L falls to minus this, every position is closed and "
                      "every pending order cancelled -- the loss is taken instead of growing. 0 = off.",
                      &s.stopLoss, 5.0, "%.2f", &settingsDirty_);
        settingDouble("First lot", "Lot size of the first order in each range.", &s.initialLot, 0.01, "%.2f",
                      &settingsDirty_);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Lot mode");
        ImGui::SetItemTooltip("How each next order's lot grows. Add: first lot + increment each time "
                              "(0.01, 0.02, 0.03...). Multiply: previous lot x multiplier (x2: 0.01, 0.02, 0.04, "
                              "0.08...).");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* modes[] = {"Add increment (0.01, 0.02, 0.03...)", "Multiply (0.01, 0.02, 0.04, 0.08...)"};
        int mode = s.lotMultiplier > 1.0 ? 1 : 0;
        if (ImGui::Combo("##lotmode", &mode, modes, 2)) {
            s.lotMultiplier = mode == 1 ? 2.0 : 1.0;
            settingsDirty_ = true;
        }
        if (s.lotMultiplier > 1.0) {
            settingDouble("Lot multiplier", "Each next order's lot = the previous one x this (2 = double).",
                          &s.lotMultiplier, 0.1, "%.2f", &settingsDirty_);
        } else {
            settingDouble("Lot increment", "Added to the lot size on every following order (0.01 -> 0.02 -> 0.03...).",
                          &s.lotIncrement, 0.01, "%.2f", &settingsDirty_);
        }
        settingDouble("Entry offset ($)",
                      "How far beyond the current price the first order sits. Its fill sets the range. "
                      "0 = place the first order at the range boundary instead.",
                      &s.seedOffset, 0.01, "%.2f", &settingsDirty_);
        settingInt("Cooldown (s)", "Pause after a basket is fully closed before the next range opens.",
                   &s.cooldownSeconds, &settingsDirty_);
        settingInt("Cooldown after Close range (s)",
                   "After you press Close range: once everything is closed, wait this long, then open a new range "
                   "around the live price (only with Auto-open on). 0 = wait for Open range instead.",
                   &s.manualCloseCooldownSeconds, &settingsDirty_);
        settingInt("Max orders", "Safety limit: no more orders are placed in a range beyond this many.", &s.maxOrders,
                   &settingsDirty_);
        settingDouble("Max lot", "Safety limit on any single order's lot size.", &s.maxLot, 0.10, "%.2f",
                      &settingsDirty_);
        settingDouble("Max basket loss ($)", "Safety limit: stop placing new orders past this floating loss. 0 = off.",
                      &s.maxBasketLoss, 10.0, "%.2f", &settingsDirty_);
        settingDouble("Daily loss limit ($)",
                      "Once today's booked + floating net reaches minus this, the running range is closed and no new "
                      "range opens until tomorrow (your computer's date). 0 = off.",
                      &s.dailyLossLimit, 10.0, "%.2f", &settingsDirty_);
        settingDouble("Commission ($ per $1M)",
                      "How cTrader brokers charge: $ per 1,000,000 USD traded, once when a position opens and again "
                      "when it closes. IC Markets Raw = 30 ($3 per $100k). One side = lots x 100 oz x entry price x "
                      "rate / 1,000,000, rounded to the cent -- exactly what cTrader shows in a position's "
                      "Commission. Used when above 0 (replaces the $/lot rate).",
                      &s.commissionPerMillion, 1.0, "%.2f", &settingsDirty_);
        ImGui::BeginDisabled(s.commissionPerMillion > 0.0);
        settingDouble("Commission ($/lot, flat)",
                      "Only for brokers that charge a fixed amount per lot: open + close per 1.00 lot. Ignored while "
                      "the $ per $1M rate is set.",
                      &s.commissionPerLot, 0.50, "%.2f", &settingsDirty_);
        ImGui::EndDisabled();
        clampSettings(s);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    renderLotPreview(editSettings_, snap && snap->bid && snap->ask ? (*snap->bid + *snap->ask) / 2.0 : 0.0);
    ImGui::BeginDisabled(!settingsLoaded_);
    ImGui::Spacing();
    ImGui::TextDisabled("Balance, leverage, margin guards, lock and escape: the Risk & guards page.");
    renderMarketSettings();
    ImGui::EndDisabled();
    renderSaveBar(controller, snap, connected);
    ImGui::EndChild();
}

void DashboardWindow::renderRiskPage(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                     bool connected) {
    ImGui::BeginChild("risk_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::BeginDisabled(!settingsLoaded_);
    renderRiskCalculator(snap);
    ImGui::EndDisabled();
    renderSaveBar(controller, snap, connected);
    ImGui::EndChild();
}

void DashboardWindow::renderSaveBar(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                    bool connected) {
    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    ImGui::BeginDisabled(!settingsDirty_ || !settingsLoaded_ || configPathBuffer_[0] == '\0');
    if (glass::button("Save & apply", kBlue, ImVec2(half, 0), glass::ButtonKind::Primary)) {
        if (controller.saveSettings(configPathBuffer_.data(), editSettings_)) {
            if (connected) controller.applySettings(editSettings_);
            settingsDirty_ = false;
            flash(connected ? "Settings saved - the bot uses them from the next range, and the app opens with them."
                            : "Settings saved - the app opens with them and the bot uses them when it connects.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Writes these settings into the config file (credentials untouched) and, if connected, "
                          "hands them to the bot for its next range.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!settingsDirty_);
    if (glass::button("Revert", glass::palette().neutral, ImVec2(half, 0))) {
        settingsDirty_ = false;
        if (connected && snap) {
            editSettings_ = snap->settings;
        } else {
            settingsLoaded_ = false;  // reload from the file next frame
        }
    }
    ImGui::EndDisabled();
}

void DashboardWindow::renderPositionsTab(LiveController& controller, const LiveSnapshot& snap, bool connected) {
    ImGui::BeginDisabled(!connected || snap.syncingPositions);
    if (glass::button(snap.syncingPositions ? "Syncing..." : "Sync from broker", kBlue)) controller.syncPositions();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Reloads every open XAUUSD position from the broker -- lists positions opened before this "
                          "session (so you can close them here) and drops ones you closed in cTrader.");
    bool anyAdopted = false;
    for (const auto& p : snap.positions) anyAdopted = anyAdopted || p.adopted;
    if (anyAdopted) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, "\"old\" = already open at the broker before this session");
    }

    if (snap.positions.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No open positions.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.3f;
    if (ImGui::BeginTable("positions", 11, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Broker ID");
        ImGui::TableSetupColumn("Lots");
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("Slippage");
        ImGui::TableSetupColumn("Now");
        ImGui::TableSetupColumn("Gross");
        ImGui::TableSetupColumn("Commission");
        ImGui::TableSetupColumn("Net");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableHeadersRow();

        for (const auto& p : snap.positions) {
            ImGui::PushID(static_cast<int>(p.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (p.adopted) {
                ImGui::TextColored(kAmber, "old");
            } else {
                ImGui::TextDisabled("%llu", static_cast<unsigned long long>(p.id));
            }
            ImGui::SetItemTooltip("Broker position id: %s", p.positionId.empty() ? "(unknown)" : p.positionId.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(p.side == OrderSide::BuyStop ? kGreen : kRed, "%s", sideName(p.side));
            ImGui::TableNextColumn();
            // The broker's own position id -- the same "ID" cTrader shows for the position.
            if (p.positionId.empty()) {
                ImGui::TextColored(kRed, "unknown");
            } else {
                ImGui::TextColored(p.verified ? kGreen : kAmber, "%s", p.positionId.c_str());
            }
            ImGui::SetItemTooltip("%s", p.verified ? "Confirmed by the broker (its fill report / position list)."
                                                   : "Not confirmed by the broker yet -- the next check (every 30 s) "
                                                     "will tell.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lots(p.lots).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(price(p.entryPrice).c_str());
            ImGui::TableNextColumn();
            // Filled vs the order's own price: + = worse (costs money).
            if (p.adopted) {
                ImGui::TextDisabled("--");
                ImGui::SetItemTooltip("Opened before the bot connected: its order price isn't known.");
            } else if (std::fabs(p.pnl.slippage) < 0.005) {
                ImGui::TextDisabled("$0.00");
            } else {
                ImGui::TextColored(p.pnl.slippage > 0 ? kRed : kGreen, "%s", money(-p.pnl.slippage).c_str());
                ImGui::SetItemTooltip("Order price %.2f, filled %.2f (%+.2f)", p.requestedPrice, p.entryPrice,
                                      p.entryPrice - p.requestedPrice);
            }
            ImGui::TableNextColumn();
            if (snap.bid && snap.ask) {
                ImGui::TextUnformatted(price(p.side == OrderSide::BuyStop ? *snap.bid : *snap.ask).c_str());
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            // Same columns as cTrader: Gross = actual fill -> closing price now
            // (bid for a buy, ask for a sell), then commission, then net.
            const double brokerGross = p.pnl.net + p.pnl.commission + p.pnl.swap;
            ImGui::TextColored(pnlColor(brokerGross), "%s", money(brokerGross).c_str());
            ImGui::SetItemTooltip("Actual fill %.2f -> closing price now, same as cTrader's Gross.\n"
                                  "  price move from the order price to mid  %s\n"
                                  "  slippage on the fill                    %s\n"
                                  "  spread (mid -> bid/ask)                 %s",
                                  p.entryPrice, money(p.pnl.gross).c_str(), money(-p.pnl.slippage).c_str(),
                                  money(-p.pnl.spread).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(p.pnl.commission > 0.005 ? kAmber : glass::palette().textDim, "%s",
                               money(-p.pnl.commission).c_str());
            ImGui::SetItemTooltip("Opening %s + closing %s = %s for %.2f lot filled at %.2f (Settings rate).\n"
                                  "The same as this position's Commission in cTrader.",
                                  money(p.pnl.commission / 2.0).c_str(), money(p.pnl.commission / 2.0).c_str(),
                                  money(p.pnl.commission).c_str(), p.lots, p.entryPrice);
            ImGui::TableNextColumn();
            withFont(boldFont_, [&] { ImGui::TextColored(pnlColor(p.pnl.net), "%s", money(p.pnl.net).c_str()); });
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            if (glass::button("Close", kRed, ImVec2(-1, 0))) controller.closePosition(p.id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    double buyLots = 0.0;
    double sellLots = 0.0;
    for (const auto& p : snap.positions) (p.side == OrderSide::BuyStop ? buyLots : sellLots) += p.lots;
    const double total = floatingPnl(snap);
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Buy %s  /  Sell %s lots   |   Gross %s   Commission %s   Net", lots(buyLots).c_str(),
                        lots(sellLots).c_str(), money(snap.live.net + snap.live.commission + snap.live.swap).c_str(),
                        money(-snap.live.commission).c_str());
    ImGui::SameLine();
    withFont(boldFont_, [&] { ImGui::TextColored(pnlColor(total), "%s", money(total).c_str()); });
}

void DashboardWindow::renderOrdersTab(LiveController& controller, const LiveSnapshot& snap, bool connected) {
    if (snap.pendingOrders.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No pending orders.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("orders", 7, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn("At broker");
        ImGui::TableSetupColumn("Lots");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 184.0f);
        ImGui::TableHeadersRow();

        for (const auto& o : snap.pendingOrders) {
            ImGui::PushID(static_cast<int>(o.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (o.adopted) {
                ImGui::TextColored(kAmber, "old");
                ImGui::SetItemTooltip("Already working at the broker when the bot connected (ClOrdID %llu) - not part "
                                      "of the current range.",
                                      static_cast<unsigned long long>(o.id));
            } else {
                ImGui::TextDisabled("%llu", static_cast<unsigned long long>(o.id));
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(o.side == OrderSide::BuyStop ? kGreen : kRed, "%s", orderTypeName(o.side));
            ImGui::TableNextColumn();
            if (o.waiting) {
                ImGui::TextDisabled("not sent");
                ImGui::SetItemTooltip("Held by the bot: the market is already past its price.");
            } else if (o.confirmed) {
                ImGui::TextColored(kGreen, "confirmed");
                ImGui::SetItemTooltip("The broker accepted it and lists it as working -- you'll see it in cTrader.");
            } else {
                ImGui::TextColored(kAmber, "sent...");
                ImGui::SetItemTooltip("Sent, the broker hasn't confirmed it yet. If it doesn't within 8 s the bot asks "
                                      "the broker and places it again if it's missing.");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lots(o.lots).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(price(o.price).c_str());
            ImGui::TableNextColumn();
            const double trigger = snap.bid && snap.ask ? (o.side == OrderSide::BuyStop ? *snap.ask : *snap.bid) : 0.0;
            if (o.waiting) {
                ImGui::TextColored(kAmber, "waiting - market is $%.2f past it", std::fabs(o.price - trigger));
                ImGui::SetItemTooltip(
                    "Not at the broker yet: the market is already past this price, so the broker would refuse a Stop "
                    "here.\nIt goes out by itself as soon as the price is back on the right side. 'Place now' "
                    "sends it at market instead.");
            } else if (snap.bid && snap.ask) {
                // How far price still has to travel to trigger it.
                ImGui::TextDisabled("$%.2f away", std::fabs(o.price - trigger));
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            const float buttonW = o.waiting ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f
                                            : -1.0f;
            if (o.waiting) {
                if (glass::button("Place now", kBlue, ImVec2(buttonW, 0))) {
                    placeNowId_ = o.id;
                    confirm_ = Confirm::PlaceNow;
                    openConfirmRequested_ = true;
                }
                ImGui::SameLine();
            }
            if (glass::button("Cancel", kAmber, ImVec2(o.waiting ? buttonW : -1.0f, 0))) controller.cancelOrder(o.id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DashboardWindow::renderLogTab(LiveController& controller) {
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##filter", "Filter log...", logFilter_.data(), logFilter_.size());
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &logAutoScroll_);
    ImGui::SameLine();
    const std::vector<std::string> lines = controller.logLines();
    if (glass::button("Copy", glass::palette().neutral)) {
        std::string all;
        for (const auto& l : lines) {
            all += l;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
        flash("Log copied to clipboard.");
    }
    ImGui::SameLine();
    if (glass::button("Save logs", kBlue, ImVec2(0, 0), glass::ButtonKind::Primary)) {
        if (const auto file = controller.saveLog(configPathBuffer_.data())) {
            flash("Log saved: " + file->string());
        } else {
            flash("Could not save the log - see the error above.");
        }
    }
    ImGui::SetItemTooltip("Writes the whole log to a .log file in the muuk_logs folder next to your config.\n"
                          "(The log is also saved there automatically every time you disconnect.)");
    ImGui::SameLine();
    if (glass::button("Logs folder", glass::palette().neutral)) {
        const std::filesystem::path dir = LiveController::dataDirectory(configPathBuffer_.data()) / "muuk_logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!openPath(dir)) flash("Could not open " + dir.string());
    }
    ImGui::SameLine();
    if (glass::button("Clear", glass::palette().neutral)) controller.clearLog();

    std::vector<const std::string*> shown;
    shown.reserve(lines.size());
    const std::string filter(logFilter_.data());
    for (const auto& l : lines) {
        if (filter.empty() || l.find(filter) != std::string::npos) shown.push_back(&l);
    }

    ImGui::BeginChild("log_lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const std::string& l = *shown[static_cast<std::size_t>(i)];
            ImVec4 color = glass::palette().text;
            if (l.find("CRITICAL") != std::string::npos || l.find("[error]") != std::string::npos ||
                l.find("rejected") != std::string::npos) {
                color = kRed;
            } else if (l.find("[manual]") != std::string::npos || l.find("[settings]") != std::string::npos) {
                color = kAmber;
            } else if (l.find("[basket]") != std::string::npos) {
                color = kViolet;
            } else if (l.find("[order]") != std::string::npos) {
                color = glass::palette().textDim;
            }
            ImGui::TextColored(color, "%s", l.c_str());
        }
    }
    if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void DashboardWindow::renderConfirmPopup(LiveController& controller) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
    const bool open = ImGui::BeginPopupModal("Please confirm", nullptr,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    if (!open) return;

    const char* title = "";
    const char* detail = "";
    const char* action = "";
    switch (confirm_) {
        case Confirm::CloseRange:
            title = "Close the running range?";
            detail = manualReopenText_.c_str();
            action = "Close range";
            break;
        case Confirm::CloseAllPositions:
            title = "Close every open position at market?";
            detail = "Pending orders stay at the broker. The bot stops trading until you open a new range.";
            action = "Close all positions";
            break;
        case Confirm::CancelAllPending:
            title = "Cancel every pending order?";
            detail = "Open positions stay open. The bot stops trading until you open a new range.";
            action = "Cancel all pending";
            break;
        case Confirm::PlaceNow:
            title = "Place the waiting order now, at market?";
            detail = "It opens right away at the current price instead of its Stop price, and the basket\n"
                     "carries on from that fill. The difference counts as slippage in the basket's P/L.";
            action = "Place now";
            break;
        case Confirm::ResetData:
            title = "Start fresh?";
            detail = "Every statistic goes back to zero: booked P/L, volume, history, speed, basket numbers,\n"
                     "reconnect counts and the log. Old history, log and reports move to archive folders next\n"
                     "to your config (nothing is deleted). Settings and anything open at the broker are not touched.";
            action = "Reset data";
            break;
        case Confirm::None:
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
    }

    withFont(boldFont_, [&] { ImGui::TextUnformatted(title); });
    ImGui::TextDisabled("%s", detail);
    ImGui::Spacing();
    ImGui::Spacing();
    if (glass::button(action, kRed, ImVec2(190, 0), glass::ButtonKind::Primary)) {
        switch (confirm_) {
            case Confirm::CloseRange:
                controller.closeRange();
                break;
            case Confirm::CloseAllPositions:
                controller.closeAllPositions();
                break;
            case Confirm::CancelAllPending:
                controller.cancelAllPendingOrders();
                break;
            case Confirm::PlaceNow:
                controller.placeWaitingOrderNow(placeNowId_);
                break;
            case Confirm::ResetData:
                if (controller.resetData(configPathBuffer_.data())) {
                    // The window's own running figures start over too.
                    rttHistory_.clear();
                    quoteHistory_.clear();
                    lastTicks_ = 0;
                    quotesPerSecond_ = 0.0;
                    lastQuoteSeenAt_ = -1.0;
                    volumeRevision_ = ~uint64_t{0};
                    dismissedResumeNote_.clear();
                    flash("Fresh start - every statistic is back to zero (old data is in the archive folders).");
                }
                break;
            case Confirm::None:
                break;
        }
        confirm_ = Confirm::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (glass::button("Keep", glass::palette().neutral, ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        confirm_ = Confirm::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}


void DashboardWindow::renderLotPreview(const LiveSettings& s, double livePrice) {
    // What the settings will actually place, order by order: lot, total
    // lots open, and where a limit stops the basket from adding more.
    const muuk::core::SymbolSpec spec;  // XAUUSD: 0.01 lot steps
    const muuk::core::LotProgressionConfig lp{muuk::core::Volume::fromDouble(s.initialLot, spec),
                                              muuk::core::Volume::fromDouble(s.lotIncrement, spec), s.lotMultiplier};
    ImGui::Spacing();
    withFont(boldFont_, [] { ImGui::TextUnformatted("Order sizes with these settings"); });
    std::string line;
    double total = 0.0;
    int limitAt = 0;
    const char* limitWhy = "";
    for (int n = 1; n <= std::min(s.maxOrders, 16); ++n) {
        const double lot = muuk::core::computeSequenceLot(lp, n, spec).toDouble(spec);
        if (lot > s.maxLot + 1e-9) {
            limitAt = n;
            limitWhy = "Max lot";
            break;
        }
        total += lot;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.2f", line.empty() ? "" : "  ", lot);
        line += buf;
        if (n == 8 && s.maxOrders > 8) {
            // Wrap for readability.
            ImGui::TextUnformatted(line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) ImGui::TextUnformatted(line.c_str());
    if (limitAt == 0 && s.maxOrders <= 16) {
        limitAt = s.maxOrders + 1;
        limitWhy = "Max orders";
    }
    ImGui::TextDisabled("Total open if all of these fill: %.2f lots", total);
    {
        // The commission the bot will book, worked out exactly like the
        // broker does -- compare with a position's Commission in cTrader.
        muuk::core::CostInputs costs;
        costs.commissionPerLotRoundTurn = s.commissionPerLot;
        costs.commissionPerMillionPerSide = s.commissionPerMillion;
        const double at = livePrice > 0.0 ? livePrice : 4000.0;
        const double first = muuk::core::roundTurnCommissionMoney(s.initialLot, at, spec, costs);
        double all = 0.0;
        for (int n = 1; n <= std::min(s.maxOrders, 16); ++n) {
            const double lot = muuk::core::computeSequenceLot(lp, n, spec).toDouble(spec);
            if (lot > s.maxLot + 1e-9) break;
            all += muuk::core::roundTurnCommissionMoney(lot, at, spec, costs);
        }
        if (s.commissionPerMillion <= 0.0 && s.commissionPerLot <= 0.0) {
            ImGui::TextColored(kAmber, "Commission is 0 - net P/L will look better than your broker's.");
        } else {
            ImGui::TextDisabled("Commission at %s%.2f: %.2f lot = $%.2f round trip (open + close), all of the above = "
                                "$%.2f -- should equal cTrader's Commission column.",
                                livePrice > 0.0 ? "" : "e.g. ", at, s.initialLot, first, all);
        }
    }
    if (limitAt > 0) {
        ImGui::TextColored(kAmber, "Order #%d would break %s -- from there the basket adds nothing more%s.", limitAt,
                           limitWhy, s.stopLoss > 0.0 ? " (the stop loss still closes it)" : ": set a Stop loss");
    }
    if (s.lotMultiplier > 1.0 && s.stopLoss <= 0.0) {
        ImGui::TextColored(kRed, "Multiplying lots without a Stop loss: one long back-and-forth can grow the loss "
                                 "very fast. Set a Stop loss.");
    }
}

void DashboardWindow::renderSpeed(const LiveSnapshot& snap) {
    ImGui::Spacing();
    withFont(boldFont_, [] { ImGui::TextUnformatted("System speed"); });
    ImGui::SameLine();
    ImGui::TextDisabled("(%llu quotes since %s)", static_cast<unsigned long long>(snap.ticksProcessed),
                        snap.statsSince.empty() ? "connect" : snap.statsSince.c_str());
    if (!ImGui::BeginTable("speed", 5, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.7f);
    ImGui::TableSetupColumn("Live");
    ImGui::TableSetupColumn("Avg");
    ImGui::TableSetupColumn("p99");
    ImGui::TableSetupColumn("Max");
    ImGui::TableHeadersRow();
    auto row = [&](const char* label, const char* help, const LatencySummary& s, bool micro) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        ImGui::SetItemTooltip("%s\n%llu samples. Avg and max are all-time (since the last reset); live and p99 "
                              "are the most recent 2048.",
                              help,
                              static_cast<unsigned long long>(s.samples));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(latency(s.last, s, micro).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(latency(s.avg, s, micro).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(latency(s.p99, s, micro).c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", latency(s.max, s, micro).c_str());
    };
    row("Quote -> decision", "The bot's own time from a price update arriving to the strategy's decision.",
        snap.tickDecisionUs, true);
    row("Fill -> next order", "The bot's own time from a fill report arriving to its reaction being sent.",
        snap.fillReactionUs, true);
    row("Broker round trip", "Order sent -> the broker's first answer (network + broker), in milliseconds.",
        snap.brokerRoundTripMs, false);
    ImGui::EndTable();
}

void DashboardWindow::renderHistoryTab(LiveController& controller, const std::optional<LiveSnapshot>& snap) {
    // One scroll for the whole tab: the summary, the per-session table and
    // every basket -- however long it gets.
    ImGui::BeginChild("history_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    const std::string configPath(configPathBuffer_.data());

    // Summary across every saved basket.
    int wins = 0;
    double net = 0.0;
    double commission = 0.0;
    double best = 0.0;
    double worst = 0.0;
    std::size_t legCount = 0;
    for (const auto& b : history_) {
        wins += b.net > 0.005 ? 1 : 0;
        net += b.net;
        commission += b.commission;
        best = std::max(best, b.net);
        worst = std::min(worst, b.net);
        legCount += b.legs.size();
    }

    if (glass::button("Export PDF report", kViolet, ImVec2(0, 0), glass::ButtonKind::Primary)) {
        if (const auto file = controller.exportReport(configPath)) {
            flash("Report saved: " + file->string());
            if (!openPath(*file)) flash("Report saved (no PDF viewer found): " + file->string());
        } else {
            flash("Could not create the report - see the error above.");
        }
    }
    ImGui::SetItemTooltip("Creates a PDF with the overview, system speed, settings, P/L charts, every basket and its "
                          "position tree, and opens it.");
    ImGui::SameLine();
    if (glass::button("Reports folder", glass::palette().neutral)) {
        const std::filesystem::path dir = LiveController::dataDirectory(configPath) / "muuk_reports";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!openPath(dir)) flash("Could not open " + dir.string());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(controller.isRunning());
    if (glass::button("Reload", glass::palette().neutral)) controller.reloadHistory(configPath);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Re-reads this account's history file (muuk_history next to the config file).");
    ImGui::SameLine();
    ImGui::BeginDisabled(configPath.empty());
    if (glass::button("Reset data", kRed)) {
        confirm_ = Confirm::ResetData;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Fresh start: archives the basket history and the log, and starts the statistics over. "
                          "Settings and anything open at the broker are not touched.");

    ImGui::Spacing();
    if (ImGui::BeginTable("hist_summary", 5, ImGuiTableFlags_SizingStretchSame)) {
        auto stat = [&](const char* caption, const std::string& value, ImVec4 color) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", caption);
            withFont(boldFont_, [&] { ImGui::TextColored(color, "%s", value.c_str()); });
        };
        const ImVec4 text = glass::palette().text;
        char winRate[48];
        std::snprintf(winRate, sizeof(winRate), "%d / %d  (%.0f%%)", wins, static_cast<int>(history_.size()),
                      history_.empty() ? 0.0 : 100.0 * wins / static_cast<double>(history_.size()));
        stat("BASKETS (ALL TIME)", std::to_string(history_.size()) + "  |  " + std::to_string(legCount) + " positions",
             text);
        stat("IN PROFIT", winRate, text);
        stat("NET P/L (ALL TIME)", money(net), pnlColor(net));
        stat("COMMISSION PAID", "$" + price(commission), commission > 0.005 ? kAmber : text);
        stat("BEST / WORST", money(best) + "  /  " + money(worst), text);
        ImGui::EndTable();
    }
    {
        // How the baskets ended, and how rough they got on the way.
        int target = 0, stop = 0, manual = 0, interrupted = 0, failed = 0, other = 0, missed = 0;
        double deepest = 0.0;
        double fillsTotal = 0.0;
        for (const auto& b : history_) {
            if (b.outcome == "target") ++target;
            else if (b.outcome == "stop loss") ++stop;
            else if (b.outcome == "manual" || b.outcome == "guard") ++manual;
            else if (b.outcome == "interrupted") ++interrupted;
            else if (b.outcome == "failed") ++failed;
            else ++other;
            missed += b.ordersMissed;
            deepest = std::min(deepest, b.worstNet);
            fillsTotal += static_cast<double>(b.legs.size());
        }
        const int n = static_cast<int>(history_.size());
        ImGui::TextDisabled("Outcomes:");
        ImGui::SameLine();
        ImGui::TextColored(kGreen, "target %d", target);
        ImGui::SameLine();
        ImGui::TextColored(stop > 0 ? kRed : glass::palette().textDim, "stop loss %d", stop);
        ImGui::SameLine();
        ImGui::TextDisabled("manual %d", manual);
        ImGui::SameLine();
        ImGui::TextColored(interrupted > 0 ? kAmber : glass::palette().textDim, "interrupted %d", interrupted);
        ImGui::SameLine();
        ImGui::TextColored(failed > 0 ? kRed : glass::palette().textDim, "failed %d", failed);
        if (other > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("older/other %d", other);
        }
        ImGui::TextDisabled("Success rate (target reached) %.0f%%   |   deepest drawdown in a basket %s   |   avg %.1f "
                            "positions per basket   |   missed orders re-placed %d",
                            n > 0 ? 100.0 * target / n : 0.0, money(deepest).c_str(), n > 0 ? fillsTotal / n : 0.0,
                            missed);
    }
    renderSessionStats();
    if (snap) {
        ImGui::TextDisabled("This session (since %s): %zu basket(s), booked net %s",
                            snap->sessionStartedAt.empty() ? "--" : snap->sessionStartedAt.c_str(),
                            snap->baskets.size(), money(snap->realizedPnl).c_str());
    }
    ImGui::Separator();

    if (snap && snap->currentBasket && !snap->currentBasket->legs.empty()) {
        const BasketRecord& b = *snap->currentBasket;
        ImGui::PushID("current");
        const bool open = ImGui::TreeNodeEx("node", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                            "Basket #%d", b.number);
        ImGui::SameLine(150.0f);
        glass::pill("in progress", kAmber, true);
        ImGui::SameLine();
        ImGui::TextColored(pnlColor(snap->live.net), "%s", money(snap->live.net).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("net now  |  %zu position(s)  |  range %s - %s  |  started %s", b.legs.size(),
                            price(b.rangeLower).c_str(), price(b.rangeUpper).c_str(), timeOf(b.startedAt).c_str());
        if (open) {
            basketLegsTable(b, true);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (history_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No closed baskets yet. Each basket is saved here (and to muuk_history next to your "
                            "config, one file per account) the moment it closes.");
    }

    // Newest first. Only the most recent ones are listed -- the PDF report
    // and the history file have all of them.
    const std::size_t shown = std::min(history_.size(), historyShown_);
    std::string lastSession;
    for (std::size_t k = 0; k < shown; ++k) {
        const std::size_t i = history_.size() - 1 - k;
        const BasketRecord& b = history_[i];
        if (k == 0 || b.session != lastSession) {
            ImGui::Spacing();
            ImGui::TextColored(kBlue, "Session started %s", b.session.empty() ? "(unknown)" : b.session.c_str());
            lastSession = b.session;
        }
        ImGui::PushID(static_cast<int>(i));
        const bool open = ImGui::TreeNodeEx("node", ImGuiTreeNodeFlags_SpanAvailWidth, "Basket #%d", b.number);
        ImGui::SameLine(150.0f);
        withFont(boldFont_, [&] { ImGui::TextColored(pnlColor(b.net), "%s", money(b.net).c_str()); });
        ImGui::SameLine(240.0f);
        ImGui::TextDisabled("%zu position(s)  |  %s  |  worst %s  |  %s  |  closed %s", b.legs.size(),
                            b.outcome.empty() ? b.closeReason.c_str() : b.outcome.c_str(), money(b.worstNet).c_str(),
                            duration(b.durationSeconds).c_str(), b.closedAt.c_str());
        if (b.ordersPlaced > 0) {
            ImGui::SetItemTooltip("%s\nOrders placed %d, filled %d, rejected %d, missed and re-placed %d\nMost lots open "
                                  "at once %.2f, best point %s",
                                  b.closeReason.c_str(), b.ordersPlaced, b.ordersFilled, b.ordersRejected,
                                  b.ordersMissed, b.maxOpenLots, money(b.bestNet).c_str());
        }
        if (b.outcome == "target" || b.outcome == "stop loss") {
            // What the bot saw when it fired vs what the closing fills really booked.
            const double exitSlip = b.net - b.netAtTrigger;
            ImGui::TextColored(std::fabs(exitSlip) < 0.005 ? glass::palette().textDim : (exitSlip < 0 ? kAmber : kGreen),
                               "%s fired at %s net, booked %s  (closing fills %s)",
                               b.outcome == "target" ? "Target" : "Stop loss", money(b.netAtTrigger).c_str(),
                               money(b.net).c_str(), money(exitSlip).c_str());
            ImGui::SetItemTooltip("Booked = what the broker really credited: every position's actual entry and exit "
                                  "fill, minus commission (open + close). It differs from the trigger only by how far "
                                  "the market moved while the closing orders were filled.");
        }
        if (open) {
            ImGui::TextDisabled("Range %s - %s   |   gross %s   commission $%.2f   net %s", price(b.rangeLower).c_str(),
                                price(b.rangeUpper).c_str(), money(b.gross).c_str(), b.commission,
                                money(b.net).c_str());
            basketLegsTable(b, false);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (history_.size() > shown) {
        ImGui::TextDisabled("%zu older basket(s)", history_.size() - shown);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show 500 more")) historyShown_ += 500;
    }
    ImGui::EndChild();
}

void DashboardWindow::selectTabByName(const std::string& name) {
    const std::pair<const char*, Tab> tabs[] = {{"positions", Tab::Positions}, {"orders", Tab::Orders},
                                                {"history", Tab::History},     {"news", Tab::News},
                                                {"risk", Tab::Risk},
                                                {"audit", Tab::Audit},         {"volume", Tab::Volume},
                                                {"performance", Tab::Performance}, {"settings", Tab::Settings},
                                                {"log", Tab::Log}};
    for (const auto& [n, t] : tabs) {
        if (name == n) selectTab_ = t;
    }
}

void DashboardWindow::requestExit(LiveController& controller) {
    if (exitApproved_) return;
    const std::optional<LiveSnapshot> snap = controller.snapshot();
    // The preview's positions are made up: nothing to guard.
    const bool exposure = !controller.demo() && snap && (!snap->positions.empty() || !snap->pendingOrders.empty());
    if (!exposure) {
        exitApproved_ = true;
        return;
    }
    openExitRequested_ = true;  // opened next frame, inside the main window
}

void DashboardWindow::renderExitPopup(LiveController& controller, const std::optional<LiveSnapshot>& snap) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
    const bool open = ImGui::BeginPopupModal("Exit guard", nullptr,
                                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    if (!open) return;

    const bool running = controller.isRunning();
    const bool connected = running && snap && snap->connected;
    const std::size_t positions = snap ? snap->positions.size() : 0;
    const std::size_t pending = snap ? snap->pendingOrders.size() : 0;
    const bool flat = snap && positions == 0 && pending == 0 && !snap->closingInProgress;

    auto finish = [&](bool approve) {
        exitClosing_ = false;
        if (approve) exitApproved_ = true;
        ImGui::CloseCurrentPopup();
    };

    if (exitClosing_) {
        const double waited = ImGui::GetTime() - exitClosingSince_;
        withFont(boldFont_, [] { ImGui::TextUnformatted("Closing everything before exit..."); });
        ImGui::TextDisabled("%zu position(s) and %zu pending order(s) left   (%.0fs)", positions, pending, waited);
        if (flat) {
            finish(true);
        } else {
            const char* problem = nullptr;
            if (!connected) {
                problem = "The connection to the broker is gone - nothing more can be closed from here.";
            } else if (snap && snap->closeFailed) {
                problem = "The broker rejected a close. Check cTrader and close what is left there.";
            } else if (waited > 30.0) {
                problem = "Still not flat after 30 seconds. Check cTrader.";
            }
            if (problem) {
                ImGui::Spacing();
                ImGui::TextColored(kRed, "%s", problem);
            }
            ImGui::Spacing();
            if (glass::button("Exit anyway", kRed, ImVec2(160, 0))) finish(true);
            ImGui::SetItemTooltip("Whatever is still open stays open at the broker.");
            ImGui::SameLine();
            if (glass::button("Back to the app", glass::palette().neutral, ImVec2(160, 0))) finish(false);
        }
        ImGui::EndPopup();
        return;
    }

    withFont(boldFont_, [] { ImGui::TextUnformatted("Trades are still open at the broker"); });
    const std::string netNow = connected ? ", net now " + money(snap->live.net) : std::string();
    ImGui::TextDisabled("%zu open position(s) and %zu pending order(s)%s.", positions, pending, netNow.c_str());
    ImGui::TextDisabled("While Muuk is closed nothing manages them: no profit target, no next order, no risk\n"
                        "limit. The basket is saved and carries on where it left off when you connect again.");
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::BeginDisabled(!connected);
    if (glass::button("Close everything & exit", kRed, ImVec2(210, 0), glass::ButtonKind::Primary)) {
        controller.closeRange(false);  // and no new range afterwards
        exitClosing_ = true;
        exitClosingSince_ = ImGui::GetTime();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", connected ? "Cancels every pending order, closes every position at market, waits for "
                                            "the broker to confirm, then exits."
                                          : "Not connected - nothing can be closed from here.");
    ImGui::SameLine();
    if (glass::button("Exit, keep them open", kAmber, ImVec2(180, 0))) finish(true);
    ImGui::SameLine();
    if (glass::button("Cancel", glass::palette().neutral, ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        finish(false);
    }
    ImGui::EndPopup();
}


void DashboardWindow::refreshVolume() {
    const std::string now = muuk::ctrader_fix::localTimestamp();
    // Recompute when the history changed, or once a minute (the day may have turned).
    if (volumeRevision_ == historyRevision_ && ImGui::GetTime() - volumeComputedAt_ < 60.0) return;
    volumeRevision_ = historyRevision_;
    volumeComputedAt_ = ImGui::GetTime();
    volumeRows_[0] = muuk::ctrader_fix::volumeByPeriod(history_, VolumePeriod::Day);
    volumeRows_[1] = muuk::ctrader_fix::volumeByPeriod(history_, VolumePeriod::Week);
    volumeRows_[2] = muuk::ctrader_fix::volumeByPeriod(history_, VolumePeriod::Month);
    volumeTotal_ = muuk::ctrader_fix::volumeTotal(history_);
    nowDay_ = muuk::ctrader_fix::periodKey(now, VolumePeriod::Day);
    nowWeek_ = muuk::ctrader_fix::periodKey(now, VolumePeriod::Week);
    nowMonth_ = muuk::ctrader_fix::periodKey(now, VolumePeriod::Month);
}

void DashboardWindow::renderVolumeTab() {
    refreshVolume();

    auto find = [](const std::vector<VolumeBucket>& rows, const std::string& key) {
        for (const auto& r : rows) {
            if (r.period == key) return r;
        }
        VolumeBucket empty;
        empty.period = key;
        return empty;
    };

    // Today / this week / this month / all time at a glance.
    if (ImGui::BeginTable("volume_summary", 4, ImGuiTableFlags_SizingStretchSame)) {
        auto stat = [&](const char* caption, const VolumeBucket& b) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", caption);
            withFont(boldFont_, [&] { ImGui::Text("%s lots", lots(b.lots).c_str()); });
            ImGui::SameLine();
            ImGui::TextColored(pnlColor(b.net), "%s", money(b.net).c_str());
            ImGui::TextDisabled("%d baskets  |  %d won / %d lost", b.baskets, b.won, b.lost);
        };
        stat("TODAY", find(volumeRows_[0], nowDay_));
        stat("THIS WEEK", find(volumeRows_[1], nowWeek_));
        stat("THIS MONTH", find(volumeRows_[2], nowMonth_));
        stat("ALL TIME", volumeTotal_);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Group by");
    ImGui::SameLine();
    ImGui::RadioButton("Day", &volumePeriod_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Week", &volumePeriod_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Month", &volumePeriod_, 2);
    ImGui::SameLine();
    ImGui::TextDisabled("   (?)");
    ImGui::SetItemTooltip(
        "Lots: every position counted once - it was opened and closed (one round turn).\n"
        "Broker volume: brokers count each side, so opening and closing = twice the lots.\n"
        "Value traded: lots x %.0f (contract size) x entry price, in USD.\n"
        "Won / lost lots: lots in baskets that closed in profit / in loss.\n"
        "Commission is worked out per position like the broker does (Settings). Baskets count on the day they closed.",
        history_.empty() ? 100.0 : history_.back().contractSize);

    const std::vector<VolumeBucket>& rows = volumeRows_[std::clamp(volumePeriod_, 0, 2)];
    if (rows.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No closed baskets yet - volume shows up here as baskets close.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("volume", 12, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(volumePeriod_ == 0 ? "Day" : (volumePeriod_ == 1 ? "Week" : "Month"),
                            ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Baskets");
    ImGui::TableSetupColumn("Won/Lost");
    ImGui::TableSetupColumn("Lots");
    ImGui::TableSetupColumn("Broker vol.");
    ImGui::TableSetupColumn("Value traded", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Won lots");
    ImGui::TableSetupColumn("Lost lots");
    ImGui::TableSetupColumn("Commission");
    ImGui::TableSetupColumn("Slippage");
    ImGui::TableSetupColumn("Net");
    ImGui::TableSetupColumn("Net/lot");
    ImGui::TableHeadersRow();

    auto row = [&](const VolumeBucket& b, bool total) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (total) {
            withFont(boldFont_, [&] { ImGui::TextUnformatted("All time"); });
        } else {
            const bool current = b.period == nowDay_ || b.period == nowWeek_ || b.period == nowMonth_;
            if (current) {
                ImGui::TextColored(kBlue, "%s", b.period.c_str());
            } else {
                ImGui::TextUnformatted(b.period.c_str());
            }
        }
        ImGui::TableNextColumn();
        ImGui::Text("%d", b.baskets);
        ImGui::TableNextColumn();
        ImGui::Text("%d / %d", b.won, b.lost);
        ImGui::TableNextColumn();
        withFont(total ? boldFont_ : nullptr, [&] { ImGui::TextUnformatted(lots(b.lots).c_str()); });
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", lots(b.sideLots()).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(dollarsWhole(b.notional).c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(kGreen, "%s", lots(b.wonLots).c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(kRed, "%s", lots(b.lostLots).c_str());
        ImGui::TableNextColumn();
        if (b.commission > 0.005) {
            ImGui::TextColored(kAmber, "-$%.2f", b.commission);
        } else {
            ImGui::TextDisabled("$0.00");
        }
        ImGui::TableNextColumn();
        if (std::fabs(b.slippage) < 0.005) {
            ImGui::TextDisabled("$0.00");
        } else {
            ImGui::TextColored(b.slippage > 0 ? kAmber : kGreen, "%s", money(-b.slippage).c_str());
            ImGui::SetItemTooltip("Fills vs order prices. Already included in Net (it comes from the real fills).");
        }
        ImGui::TableNextColumn();
        withFont(boldFont_, [&] { ImGui::TextColored(pnlColor(b.net), "%s", money(b.net).c_str()); });
        ImGui::TableNextColumn();
        ImGui::TextColored(pnlColor(b.netPerLot()), "%s", money(b.netPerLot()).c_str());
    };
    for (const auto& b : rows) row(b, false);
    row(volumeTotal_, true);
    ImGui::EndTable();
}


void DashboardWindow::tickWithoutDrawing(LiveController& controller) {
    controller.setAutoReconnect(prefs_.autoReconnect);
    controller.tick();
}

void DashboardWindow::samplePerformance(const std::optional<LiveSnapshot>& snap) {
    monitor_.sample(LiveController::dataDirectory(configPathBuffer_.data()));
    const double now = ImGui::GetTime();
    if (now - perfSampledAt_ < 1.0) return;
    const double elapsed = perfSampledAt_ < 0 ? 1.0 : now - perfSampledAt_;
    perfSampledAt_ = now;
    const bool connected = snap && snap->connected;
    if (connected) {
        const uint64_t ticks = snap->ticksProcessed;
        quotesPerSecond_ = lastTicks_ > 0 && ticks >= lastTicks_ ? static_cast<double>(ticks - lastTicks_) / elapsed : 0.0;
        lastTicks_ = ticks;
        if (quotesPerSecond_ > 0.0) lastQuoteSeenAt_ = now;
        if (snap->brokerRoundTripMs.samples > 0) rttHistory_.push_back(static_cast<float>(snap->brokerRoundTripMs.last));
        quoteHistory_.push_back(static_cast<float>(quotesPerSecond_));
    } else {
        lastTicks_ = 0;
        quotesPerSecond_ = 0.0;
    }
    while (rttHistory_.size() > SystemMonitor::kHistory) rttHistory_.pop_front();
    while (quoteHistory_.size() > SystemMonitor::kHistory) quoteHistory_.pop_front();
}

void DashboardWindow::renderPerformanceTab(LiveController& controller, const std::optional<LiveSnapshot>& snap) {
    const SystemMonitor::Sample& m = monitor_.latest();
    const bool connected = snap && snap->connected;
    const LiveController::ConnectionHealth health = controller.connectionHealth();

    auto duration = [](double seconds) {
        char buf[48];
        const long long s = static_cast<long long>(seconds);
        if (s >= 86400) {
            std::snprintf(buf, sizeof(buf), "%lldd %lldh %02lldm", s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
        } else if (s >= 3600) {
            std::snprintf(buf, sizeof(buf), "%lldh %02lldm", s / 3600, (s % 3600) / 60);
        } else {
            std::snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
        }
        return std::string(buf);
    };

    // --- Verdict: what, if anything, needs attention on this machine.
    std::vector<std::pair<ImVec4, std::string>> issues;
    if (!connected) {
        issues.push_back({kGrey, health.reconnectInSeconds
                                     ? "Not connected - reconnecting in " + std::to_string(*health.reconnectInSeconds) + " s"
                                     : std::string("Not connected")});
    }
    if (connected && snap->brokerRoundTripMs.samples > 0) {
        const double avg = snap->brokerRoundTripMs.avg;
        if (avg > 150.0) {
            issues.push_back({kRed, "Broker round trip is slow (" + price(avg) +
                                        " ms avg). A VPS closer to the broker's servers (London) would help."});
        } else if (avg > 50.0) {
            issues.push_back({kAmber, "Broker round trip " + price(avg) + " ms avg - fine, but a closer VPS is faster."});
        }
    }
    if (m.cpuSystemPct > 85.0) issues.push_back({kRed, "The machine's CPU is nearly maxed out (" + price(m.cpuSystemPct) + "%)."});
    if (m.systemMemoryUsedPct > 90.0) issues.push_back({kRed, "Memory is nearly full (" + price(m.systemMemoryUsedPct) + "% used)."});
    if (m.diskTotalGb > 0 && m.diskFreeGb < 2.0) issues.push_back({kRed, "Less than 2 GB free on the data drive."});
    if (connected && snap->clockOffsetMs && std::fabs(*snap->clockOffsetMs) > 2000.0) {
        issues.push_back({kAmber, "This machine's clock is " + price(std::fabs(*snap->clockOffsetMs) / 1000.0) +
                                      " s off the broker's - turn on Windows time sync (Settings > Time > Sync now)."});
    }
    if (connected && lastQuoteSeenAt_ > 0 && ImGui::GetTime() - lastQuoteSeenAt_ > 60.0) {
        issues.push_back({kAmber, "No quotes for over a minute (market closed, or the price feed stalled)."});
    }
    if (connected && issues.empty()) {
        glass::pill("Healthy - ready for trading", kGreen);
    } else {
        for (const auto& [color, text] : issues) ImGui::TextColored(color, "%s", text.c_str());
    }
    ImGui::Spacing();

    // --- Four live panels with 5-minute graphs.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - spacing * 3) / 4.0f;
    const float h = ImGui::GetTextLineHeight() * 3.0f + (numberFont_ ? numberFont_->FontSize : 20.0f) +
                    ImGui::GetStyle().WindowPadding.y * 2 + ImGui::GetStyle().ItemSpacing.y * 4 +
                    (liteMode() ? 0.0f : 46.0f);
    auto pct = [](double v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f%%", v);
        return std::string(buf);
    };
    auto panel = [&](const char* id, const char* caption, const std::string& value, ImVec4 color,
                     const std::string& sub, const std::deque<float>& graph, float graphMax, bool last) {
        glass::beginCard(id, ImVec2(w, h), false, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("%s", caption);
        withFont(numberFont_, [&] { ImGui::TextColored(color, "%s", value.c_str()); });
        ImGui::TextDisabled("%s", sub.c_str());
        if (!liteMode()) {  // lite: numbers only
            std::vector<float> values(graph.begin(), graph.end());
            if (values.empty()) values.push_back(0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
            ImGui::PlotLines("##g", values.data(), static_cast<int>(values.size()), 0, nullptr, 0.0f, graphMax,
                             ImVec2(-FLT_MIN, 42.0f));
            ImGui::PopStyleColor(2);
        }
        glass::endCard();
        if (!last) ImGui::SameLine();
    };

    const auto& rtt = snap ? snap->brokerRoundTripMs : muuk::ctrader_fix::LatencySummary{};
    float rttMax = 20.0f;
    for (const float v : rttHistory_) rttMax = std::max(rttMax, v * 1.2f);
    panel("p_rtt", "BROKER ROUND TRIP", rtt.samples ? price(rtt.last) + " ms" : std::string("--"),
          rtt.samples == 0 ? kGrey : (rtt.avg > 150 ? kRed : (rtt.avg > 50 ? kAmber : kGreen)),
          rtt.samples ? "avg " + price(rtt.avg) + "  p99 " + price(rtt.p99) + "  max " + price(rtt.max) + " ms"
                      : std::string("measured on every order"),
          rttHistory_, rttMax, false);
    panel("p_cpu", "CPU (MACHINE)", pct(m.cpuSystemPct),
          m.cpuSystemPct > 85 ? kRed : (m.cpuSystemPct > 60 ? kAmber : kGreen),
          "this app " + pct(m.cpuProcessPct) + " | " + std::to_string(m.cpuCores) + " cores",
          monitor_.cpuHistory(), 100.0f, false);
    std::deque<float> memGraph;  // flat line -- memory barely moves; the number is what matters
    panel("p_mem", "MEMORY", pct(m.systemMemoryUsedPct),
          m.systemMemoryUsedPct > 90 ? kRed : (m.systemMemoryUsedPct > 75 ? kAmber : kGreen),
          "of " + price(m.systemMemoryTotalGb) + " GB | this app " +
              std::to_string(static_cast<int>(m.processMemoryMb)) + " MB",
          memGraph,
          100.0f, false);
    float qMax = 5.0f;
    for (const float v : quoteHistory_) qMax = std::max(qMax, v * 1.2f);
    panel("p_quotes", "QUOTES / SECOND", connected ? price(quotesPerSecond_) : std::string("--"), kBlue,
          snap ? std::to_string(snap->ticksProcessed) + " processed" : std::string(""), quoteHistory_, qMax, true);

    ImGui::Spacing();

    // --- Details.
    if (ImGui::BeginTable("perf_details", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        withFont(boldFont_, [] { ImGui::TextUnformatted("Connection"); });
        auto row = [](const char* label, const std::string& value, ImVec4 color = ImVec4(0, 0, 0, 0)) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(190.0f);
            if (color.w > 0) {
                ImGui::TextColored(color, "%s", value.c_str());
            } else {
                ImGui::TextUnformatted(value.c_str());
            }
        };
        row("Status", connected ? "Live" : (controller.isRunning() ? "Connecting" : "Offline"),
            connected ? kGreen : kGrey);
        row("Connected since", connected && !snap->connectedSince.empty() ? snap->connectedSince : std::string("--"));
        row("Reconnects", std::to_string(health.reconnects) +
                              (health.reconnectInSeconds ? "  (next in " + std::to_string(*health.reconnectInSeconds) + " s)"
                                                         : std::string()));
        row("Last drop", health.lastDrop.empty() ? std::string("none") : health.lastDropAt + " - " + health.lastDrop,
            health.lastDrop.empty() ? ImVec4(0, 0, 0, 0) : kAmber);
        if (snap && snap->clockOffsetMs) {
            const double off = *snap->clockOffsetMs;
            row("Clock vs broker", (off >= 0 ? "+" : "") + price(off) + " ms",
                std::fabs(off) > 2000 ? kAmber : ImVec4(0, 0, 0, 0));
            ImGui::SetItemTooltip("This machine's clock minus the broker's timestamp on its quotes (so it also "
                                  "includes the one-way network delay). Within a few hundred ms is fine.");
        } else {
            row("Clock vs broker", "--");
        }
        if (ImGui::Checkbox("Reconnect by itself if the connection drops", &prefs_.autoReconnect)) savePrefs(prefs_);
        ImGui::BeginDisabled(softwareRenderer_);
        bool lite = liteMode();
        if (ImGui::Checkbox("Lite mode (least CPU)", &lite)) {
            prefs_.liteMode = lite;
            savePrefs(prefs_);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(softwareRenderer_
                                  ? "Always on here: there's no graphics card, so every frame is drawn by the CPU."
                                  : "Flat look, no animations, and the window redraws only when something happens "
                                    "(about once a second when idle). Trading is never slowed either way.");
        ImGui::SetItemTooltip("Retries after 5, 10, 20, 40, then every 60 s. The running basket is checked against "
                              "the broker and picked up again on reconnect.");

        ImGui::TableNextColumn();
        withFont(boldFont_, [] { ImGui::TextUnformatted("Machine"); });
        row("Machine up", duration(m.machineUptimeHours * 3600.0));
        row("App up", duration(monitor_.appUptimeSeconds()));
        row("Data drive", price(m.diskFreeGb) + " GB free of " + price(m.diskTotalGb) + " GB",
            m.diskTotalGb > 0 && m.diskFreeGb < 2.0 ? kRed : ImVec4(0, 0, 0, 0));
        if (snap) {
            row("Decision per quote", latency(snap->tickDecisionUs.avg, snap->tickDecisionUs, true) + " avg, " +
                                          latency(snap->tickDecisionUs.p99, snap->tickDecisionUs, true) + " p99");
            row("Reaction to a fill", latency(snap->fillReactionUs.avg, snap->fillReactionUs, true) + " avg");
        }
        row("Graphics", rendererName_.empty() ? std::string("--") : rendererName_);
        row("Data folder", LiveController::dataDirectory(configPathBuffer_.data()).string());
        ImGui::EndTable();
    }
}


namespace {
template <std::size_t N>
void copyTo(std::array<char, N>& buf, const std::string& s) {
    const std::size_t n = std::min(s.size(), N - 1);
    std::copy_n(s.begin(), n, buf.begin());
    buf[n] = '\0';
}
}  // namespace

void DashboardWindow::loadAccountForm(const std::string& configPath) {
    accountLoadedFor_ = configPath;
    accountDirty_ = false;
    accountMessage_.clear();
    AccountForm f;
    try {
        const muuk::ctrader_fix::AccountSettings a = muuk::ctrader_fix::accountFromConfigFile(configPath);
        f.live = a.environment == "live";
        copyTo(f.brokerUid, a.brokerUid);
        copyTo(f.traderLogin, a.traderLogin);
        copyTo(f.targetCompId, a.targetCompId);
        copyTo(f.quoteHost, a.quoteHost);
        copyTo(f.quotePort, a.quotePort);
        copyTo(f.tradeHost, a.tradeHost);
        copyTo(f.tradePort, a.tradePort);
        copyTo(f.symbolName, a.symbolName);
        copyTo(f.symbolId, a.symbolId);
        f.platform = a.platform == "mt5" ? 1 : 0;
        f.mt5EaPort = a.mt5EaPort;
        f.mt5FixPort = a.mt5FixPort;
        bridgeEaPortHint_ = a.mt5EaPort;
        f.loaded = true;
    } catch (const std::exception&) {
        f.loaded = false;  // no such file yet / not a config -- the form stays empty
    }
    account_ = f;
}

muuk::ctrader_fix::AccountSettings DashboardWindow::accountFromForm() const {
    muuk::ctrader_fix::AccountSettings a;
    a.environment = account_.live ? "live" : "demo";
    a.brokerUid = account_.brokerUid.data();
    a.traderLogin = account_.traderLogin.data();
    a.password = account_.password.data();
    a.targetCompId = account_.targetCompId.data();
    a.quoteHost = account_.quoteHost.data();
    a.quotePort = account_.quotePort.data();
    a.tradeHost = account_.tradeHost.data();
    a.tradePort = account_.tradePort.data();
    a.symbolName = account_.symbolName.data();
    a.symbolId = account_.symbolId.data();
    a.platform = account_.platform == 1 ? "mt5" : "ctrader";
    a.mt5EaPort = account_.mt5EaPort;
    a.mt5FixPort = account_.mt5FixPort;
    return a;
}

void DashboardWindow::renderMt5AccountFields() {
    ImGui::PushStyleColor(ImGuiCol_Text, glass::palette().textDim);
    ImGui::TextWrapped("MetaTrader 5 through the Muuk bridge: MuukBridge (tools/mt5) runs in MT5 on this PC and "
                       "connects to the app -- no FIX login. Use a HEDGING account. Cent accounts work: amounts are "
                       "then in US cents. Setup: docs/mt5.md.");
    ImGui::PopStyleColor();
    if (ImGui::BeginTable("account_mt5", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        auto row = [&](const char* label, const char* help) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (help) ImGui::SetItemTooltip("%s", help);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        row("Account type", nullptr);
        int env = account_.live ? 1 : 0;
        if (ImGui::Combo("##env5", &env, "Demo\0LIVE (real money)\0")) {
            account_.live = env == 1;
            accountDirty_ = true;
        }
        row("MT5 account number", "Only a label here (the EA reports the real one).");
        if (ImGui::InputText("##login5", account_.traderLogin.data(), account_.traderLogin.size(),
                             ImGuiInputTextFlags_CharsDecimal)) {
            accountDirty_ = true;
        }
        row("Symbol", "Keep XAUUSD -- the bridge maps it to the chart's symbol (XAUUSDc, XAUUSD.m, ...).");
        if (ImGui::InputText("##sym5", account_.symbolName.data(), account_.symbolName.size())) accountDirty_ = true;
        row("EA port", "MuukBridge's BridgePort input must be the same (default 5200).");
        if (ImGui::InputInt("##eaport", &account_.mt5EaPort, 0)) accountDirty_ = true;
        row("Bot port", "Local port between the bot and the bridge (default 5201).");
        if (ImGui::InputInt("##fixport", &account_.mt5FixPort, 0)) accountDirty_ = true;
        ImGui::EndTable();
    }
}

void DashboardWindow::renderAccountSection(bool running) {
    ImGui::SetNextItemOpen(!running, ImGuiCond_Appearing);
    if (!ImGui::CollapsingHeader("Account (broker login)")) return;

    const std::string configPath(configPathBuffer_.data());
    ImGui::TextDisabled("File: %s", configPath.empty() ? "(none)" : configPath.c_str());
    if (running) {
        ImGui::TextColored(kAmber, "Disconnect first to change the account.");
    }
    ImGui::BeginDisabled(running);

    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Platform");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##platform", &account_.platform, "cTrader (FIX API)\0MetaTrader 5 (Muuk bridge)\0")) {
            accountDirty_ = true;
        }
    }
    if (account_.platform == 1) {
        renderMt5AccountFields();
    } else {

    // Paste from cTrader: Settings > FIX API shows "SenderCompID" as demo.broker.12345.
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##sendercompid", "Paste SenderCompID from cTrader (e.g. demo.icmarkets.12345678)",
                                 account_.pasteSenderCompId.data(), account_.pasteSenderCompId.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit() && account_.pasteSenderCompId[0] != '\0')) {
        muuk::ctrader_fix::AccountSettings a = accountFromForm();
        if (a.setFromSenderCompId(account_.pasteSenderCompId.data())) {
            account_.live = a.environment == "live";
            copyTo(account_.brokerUid, a.brokerUid);
            copyTo(account_.traderLogin, a.traderLogin);
            accountDirty_ = true;
            accountMessage_.clear();
        } else {
            accountMessage_ = "That doesn't look like a SenderCompID (environment.broker.account).";
        }
        account_.pasteSenderCompId[0] = '\0';
    }

    auto field = [&](const char* label, const char* help, char* buf, std::size_t size, ImGuiInputTextFlags flags = 0,
                     const char* hint = nullptr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (help) ImGui::SetItemTooltip("%s", help);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(label);
        const bool changed = hint ? ImGui::InputTextWithHint("##v", hint, buf, size, flags)
                                  : ImGui::InputText("##v", buf, size, flags);
        if (changed) accountDirty_ = true;
        ImGui::PopID();
    };

    if (ImGui::BeginTable("account", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Account type");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        int env = account_.live ? 1 : 0;
        const char* envs[] = {"Demo", "LIVE (real money)"};
        if (ImGui::Combo("##env", &env, envs, 2)) {
            account_.live = env == 1;
            accountDirty_ = true;
        }
        field("Account number", "The trading account / login number (the last part of SenderCompID).",
              account_.traderLogin.data(), account_.traderLogin.size(), ImGuiInputTextFlags_CharsDecimal);
        field("Password", "The FIX API password from cTrader (Settings > FIX API). Never shown; leave empty to keep the "
                          "saved one.",
              account_.password.data(), account_.password.size(), ImGuiInputTextFlags_Password,
              "unchanged (leave empty to keep)");
        field("Broker UID", "The middle part of SenderCompID, e.g. icmarkets.", account_.brokerUid.data(),
              account_.brokerUid.size());
        field("TargetCompID", "As shown in cTrader's FIX API settings (often \"cServer\").",
              account_.targetCompId.data(), account_.targetCompId.size());
        field("Price host", "Price (quote) connection host.", account_.quoteHost.data(), account_.quoteHost.size());
        field("Price port", "Price (quote) connection port (plain text port).", account_.quotePort.data(),
              account_.quotePort.size(), ImGuiInputTextFlags_CharsDecimal);
        field("Trade host", "Trade connection host.", account_.tradeHost.data(), account_.tradeHost.size());
        field("Trade port", "Trade connection port (plain text port).", account_.tradePort.data(),
              account_.tradePort.size(), ImGuiInputTextFlags_CharsDecimal);
        field("Symbol", "The symbol to trade, as the broker names it.", account_.symbolName.data(),
              account_.symbolName.size());
        field("Symbol id", "Optional. Empty = looked up by name when connecting (needed for a new broker).",
              account_.symbolId.data(), account_.symbolId.size(), ImGuiInputTextFlags_CharsDecimal,
              "empty = look up by name");
        ImGui::EndTable();
    }
    }  // cTrader fields
    if (account_.live) {
        ImGui::TextColored(kRed, "LIVE account: every order uses real money. Set a Stop loss and test on demo first.");
    }

    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    ImGui::BeginDisabled(!accountDirty_ || configPath.empty());
    if (glass::button("Save to this file", kBlue, ImVec2(third, 0), glass::ButtonKind::Primary)) {
        try {
            muuk::ctrader_fix::saveAccountSettings(configPath, accountFromForm());
            loadAccountForm(configPath);
            flash("Account saved to " + configPath + " - press Connect.");
        } catch (const std::exception& ex) {
            accountMessage_ = ex.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Replaces the login in the current config file. Its saved basket state stays tied to the old "
                          "account and is ignored for the new one.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(third);
    ImGui::InputTextWithHint("##newfile", "new file, e.g. live_account", account_.newFileName.data(),
                             account_.newFileName.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(account_.newFileName[0] == '\0' || configPath.empty());
    if (glass::button("Save as new account", kViolet, ImVec2(third, 0))) {
        std::string name = account_.newFileName.data();
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json") name += ".local.json";
        const std::filesystem::path target = LiveController::dataDirectory(configPath) / name;
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            accountMessage_ = target.filename().string() + " already exists - pick another name.";
        } else {
            try {
                muuk::ctrader_fix::saveAccountSettings(target, accountFromForm(), std::filesystem::path(configPath));
                setConfigPath(target.string());
                loadAccountForm(target.string());
                flash("New account file " + target.filename().string() +
                      " created with your strategy settings - press Connect.");
            } catch (const std::exception& ex) {
                accountMessage_ = ex.what();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Creates a new config file next to this one with these login details and the current strategy "
                          "settings, and switches to it. Each account file keeps its own history and state. To go back, "
                          "pick the other file in 'Config file' at the top.");
    if (!accountMessage_.empty()) ImGui::TextColored(kRed, "%s", accountMessage_.c_str());
    ImGui::EndDisabled();
    ImGui::Separator();
}


// ---------------------------------------------------------------------------
// Market hours, news, auto target

MarketStatus DashboardWindow::currentMarket(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                            bool connected) {
    if (connected && snap && snap->marketKnown) return snap->market;
    const double now = ImGui::GetTime();
    if (now - uiMarketAt_ < 1.0) return uiMarket_;
    const MarketHoursConfig& cfg = editSettings_.market;
    const std::string key = muuk::ctrader_fix::marketHoursToJson(cfg).dump();
    std::vector<NewsEvent> feed;
    if (const auto news = controller.news()) {
        for (auto& e : news->events()) {
            if (muuk::ctrader_fix::newsIsGuarded(e, cfg)) feed.push_back(std::move(e));
        }
    }
    const int64_t ms = muuk::ctrader_fix::utcNowMs();
    const bool full = key != uiMarketKey_ || now - uiMarketFullAt_ >= 60.0 ||
                      (uiMarket_.nextChangeUtcMs > 0 && ms >= uiMarket_.nextChangeUtcMs);
    MarketStatus st = muuk::ctrader_fix::evaluateMarket(ms, cfg, &feed, full);
    if (full) {
        uiMarketFullAt_ = now;
    } else {
        st.nextChangeUtcMs = uiMarket_.nextChangeUtcMs;
        st.nextChangeLabel = uiMarket_.nextChangeLabel;
        st.hasNextNews = uiMarket_.hasNextNews;
        st.nextNews = uiMarket_.nextNews;
    }
    uiMarket_ = std::move(st);
    uiMarketAt_ = now;
    uiMarketKey_ = key;
    return uiMarket_;
}

void DashboardWindow::quickApply(LiveController& controller, const std::optional<LiveSnapshot>& snap, bool connected,
                                 const std::function<void(LiveSettings&)>& change) {
    LiveSettings s = connected && snap ? snap->settings : editSettings_;
    change(s);
    clampSettings(s);
    if (connected) controller.applySettings(s);
    if (configPathBuffer_[0] != '\0') (void)controller.saveSettings(configPathBuffer_.data(), s);
    change(editSettings_);  // the editor shows it too; other unsaved edits stay
    uiMarketAt_ = -1000.0;  // re-evaluate the market straight away
}

void DashboardWindow::renderMarketStrip(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                        bool connected) {
    const MarketStatus m = currentMarket(controller, snap, connected);
    const LiveSettings& s = connected && snap ? snap->settings : editSettings_;
    const int64_t now = muuk::ctrader_fix::utcNowMs();

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Session");
    ImGui::SameLine();
    glass::pill(m.sessionLabel.c_str(), m.marketOpen ? kBlue : kGrey);
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("Trading sessions in your time (daylight saving included):");
        for (int i = 0; i < muuk::ctrader_fix::kSessionCount; ++i) {
            const auto w = muuk::ctrader_fix::sessionWindow(static_cast<Session>(i), now);
            ImGui::Text("%-9s %s - %s%s", muuk::ctrader_fix::sessionName(static_cast<Session>(i)),
                        muuk::ctrader_fix::formatLocalDateTime(w.openUtcMs).c_str(),
                        muuk::ctrader_fix::formatLocalDateTime(w.closeUtcMs).substr(11).c_str(),
                        w.openNow ? "   (open now)" : "");
        }
        ImGui::TextDisabled("Gold: Sunday 18:00 to Friday 17:00 New York time, daily break 17:00-18:00.");
        ImGui::EndTooltip();
    }

    ImGui::SameLine(0.0f, 14.0f);
    ImGui::TextDisabled("Market");
    ImGui::SameLine();
    std::string label = m.label;
    if (!s.market.enabled) label = m.marketOpen ? "Filter off" : m.label + " (filter off)";
    glass::pill(label.c_str(), s.market.enabled ? phaseColor(m.phase) : kGrey,
                m.phase == MarketPhase::NewsPause && s.market.enabled);
    ImGui::SetItemTooltip("%s%s", m.detail.c_str(),
                          s.market.enabled ? "\nA running range keeps being managed; only new ranges wait."
                                           : "\nMarket hours filter is off (Settings > Market hours & news).");
    if (s.market.enabled && m.nextChangeUtcMs > 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        const int64_t left = m.nextChangeUtcMs - now;
        const bool soon = m.canOpenRange && left < 15 * 60'000;
        ImGui::TextColored(soon ? kAmber : glass::palette().textDim, "%s in %s", m.nextChangeLabel.c_str(),
                           countdown(left).c_str());
        ImGui::SetItemTooltip("At %s (your time).", muuk::ctrader_fix::formatLocalDateTime(m.nextChangeUtcMs).c_str());
    }

    ImGui::SameLine(0.0f, 18.0f);
    bool guard = s.market.newsGuard;
    if (ImGui::Checkbox("News guard", &guard)) {
        quickApply(controller, snap, connected, [guard](LiveSettings& x) { x.market.newsGuard = guard; });
    }
    ImGui::SetItemTooltip("On: no new range from %d min before to %d min after %s%s news for %s (ForexFactory "
                          "calendar). Off: news is ignored.",
                          s.market.newsMinutesBefore, s.market.newsMinutesAfter, s.market.newsHigh ? "red" : "",
                          s.market.newsMedium ? (s.market.newsHigh ? " and orange" : "orange") : "",
                          s.market.newsCurrencies.c_str());
    ImGui::SameLine();
    if (!guard) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, "news ignored");
    } else if (m.hasNextNews) {
        const NewsEvent& n = m.nextNews;
        const int64_t to = n.utcMs - now;
        const bool inWindow = m.phase == MarketPhase::NewsPause;
        const std::string text = std::string(inWindow ? "NEWS NOW: " : "") + (n.currency.empty() ? "" : n.currency + " ") +
                                 n.title + (to > 0 ? "  in " + countdown(to) : "  " + countdown(-to) + " ago");
        glass::pill(text.c_str(), impactColor(n.impact), inWindow);
        if (ImGui::IsItemClicked()) selectTab_ = Tab::News;
        ImGui::SetItemTooltip("%s impact, %s (your time)%s%s%s%s\nNew ranges pause %d min before to %d min after. "
                              "Click for the News tab.",
                              n.impact.c_str(), muuk::ctrader_fix::formatLocalDateTime(n.utcMs).c_str(),
                              n.forecast.empty() ? "" : "\nForecast ", n.forecast.c_str(),
                              n.previous.empty() ? "" : "   Previous ", n.previous.c_str(), s.market.newsMinutesBefore,
                              s.market.newsMinutesAfter);
    } else {
        ImGui::AlignTextToFramePadding();
        const auto news = controller.news();
        const auto st = news ? news->status() : muuk::ctrader_fix::NewsFeed::Status{};
        if (!st.loaded) {
            ImGui::TextColored(st.error.empty() ? glass::palette().textDim : kAmber, "%s",
                               st.error.empty() ? "loading ForexFactory calendar..." : st.error.c_str());
        } else {
            ImGui::TextDisabled("no guarded news ahead this week");
        }
    }
}

void DashboardWindow::renderNewsTab(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                    bool connected) {
    const auto feed = controller.news();
    const LiveSettings& s = connected && snap ? snap->settings : editSettings_;
    const MarketHoursConfig& mc = s.market;
    const int64_t now = muuk::ctrader_fix::utcNowMs();

    bool guard = mc.newsGuard;
    if (ImGui::Checkbox("News guard", &guard)) {
        quickApply(controller, snap, connected, [guard](LiveSettings& x) { x.market.newsGuard = guard; });
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (guard) {
        ImGui::TextDisabled("No new range from %d min before to %d min after %s%s%s news for %s. A running range keeps "
                            "being managed.",
                            mc.newsMinutesBefore, mc.newsMinutesAfter, mc.newsHigh ? "red" : "",
                            mc.newsHigh && mc.newsMedium ? " + " : "", mc.newsMedium ? "orange" : "",
                            mc.newsCurrencies.c_str());
    } else {
        ImGui::TextColored(kAmber, "Off: the bot trades straight through the news.");
    }

    const auto st = feed ? feed->status() : muuk::ctrader_fix::NewsFeed::Status{};
    ImGui::AlignTextToFramePadding();
    if (st.loaded) {
        ImGui::TextDisabled("ForexFactory calendar: %zu events this week%s%s", st.events,
                            st.updatedAt.empty() ? "" : ", downloaded ", st.updatedAt.c_str());
    } else {
        ImGui::TextDisabled("ForexFactory calendar: not loaded yet");
    }
    if (!st.error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "(%s)", st.error.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!feed);
    if (ImGui::SmallButton("Refresh")) feed->refreshNow();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Downloads the calendar again (ForexFactory allows it at most every 5 minutes; it's "
                          "refreshed by itself every hour).");
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::Checkbox("USD only", &newsUsdOnly_);
    ImGui::SameLine();
    ImGui::Checkbox("Hide low impact", &newsHideLow_);

    std::vector<NewsEvent> events = feed ? feed->events() : std::vector<NewsEvent>{};
    for (const auto& n : mc.news) events.push_back(n);
    std::sort(events.begin(), events.end(), [](const NewsEvent& a, const NewsEvent& b) { return a.utcMs < b.utcMs; });

    const float formHeight = ImGui::GetFrameHeightWithSpacing() * 2.6f;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("news", 8, flags, ImVec2(0, -formHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time (yours)", ImGuiTableColumnFlags_WidthFixed, 118.0f);
        ImGui::TableSetupColumn("In", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        ImGui::TableSetupColumn("Cur", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Impact", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Forecast", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Guard", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();
        int shown = 0;
        int index = 0;
        for (const auto& n : events) {
            ++index;
            if (n.utcMs < now - 2 * 3'600'000) continue;  // earlier today and before: gone
            if (!n.manual && newsUsdOnly_ && n.currency != "USD") continue;
            if (!n.manual && newsHideLow_ && n.impact != "High" && n.impact != "Medium") continue;
            ++shown;
            const bool guarded = mc.newsGuard && muuk::ctrader_fix::newsIsGuarded(n, mc);
            const bool inWindow = guarded && now >= n.utcMs - mc.newsMinutesBefore * 60'000LL &&
                                  now < n.utcMs + mc.newsMinutesAfter * 60'000LL;
            const bool past = n.utcMs <= now;
            ImGui::PushID(index);
            ImGui::TableNextRow();
            if (inWindow) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(withAlpha(kRed, 0.14f)));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const std::string when = muuk::ctrader_fix::formatLocalDateTime(n.utcMs);
            if (past) {
                ImGui::TextDisabled("%s", when.substr(5).c_str());
            } else {
                ImGui::TextUnformatted(when.substr(5).c_str());
            }
            ImGui::TableNextColumn();
            if (inWindow) {
                ImGui::TextColored(kRed, "%s", past ? "GUARD" : ("NOW " + countdown(n.utcMs - now)).c_str());
            } else if (past) {
                ImGui::TextDisabled("done");
            } else {
                const bool soon = n.utcMs - now < 60 * 60'000;
                ImGui::TextColored(soon ? impactColor(n.impact) : glass::palette().text, "%s",
                                   countdown(n.utcMs - now).c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(n.currency.empty() ? "-" : n.currency.c_str());
            ImGui::TableNextColumn();
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetTextLineHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5.0f, p.y + h * 0.5f + 1.0f), 5.0f,
                                                            ImGui::GetColorU32(impactColor(n.impact)), 12);
                ImGui::Dummy(ImVec2(13.0f, h));
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextColored(impactColor(n.impact), "%s", n.impact.empty() ? "-" : n.impact.c_str());
            }
            ImGui::TableNextColumn();
            if (past && !inWindow) {
                ImGui::TextDisabled("%s%s", n.title.c_str(), n.manual ? "  (yours)" : "");
            } else {
                ImGui::Text("%s%s", n.title.c_str(), n.manual ? "  (yours)" : "");
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(n.forecast.empty() ? "" : n.forecast.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", n.previous.c_str());
            ImGui::TableNextColumn();
            if (n.manual) {
                if (ImGui::SmallButton("Remove")) {
                    const int64_t at = n.utcMs;
                    const std::string title = n.title;
                    quickApply(controller, snap, connected, [at, title](LiveSettings& x) {
                        auto& v = x.market.news;
                        v.erase(std::remove_if(v.begin(), v.end(),
                                               [&](const NewsEvent& e) { return e.utcMs == at && e.title == title; }),
                                v.end());
                    });
                }
            } else if (guarded) {
                ImGui::TextColored(impactColor(n.impact), "pause");
                ImGui::SetItemTooltip("No new range %d min before to %d min after.", mc.newsMinutesBefore,
                                      mc.newsMinutesAfter);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (shown == 0) ImGui::TextDisabled("Nothing to show with these filters.");
    }

    // Add a release by hand (anything ForexFactory doesn't list, e.g. a speech).
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Add by hand");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputTextWithHint("##newswhen", "YYYY-MM-DD HH:MM", newsWhen_.data(), newsWhen_.size());
    ImGui::SetItemTooltip("Your computer's local time.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 90.0f));
    ImGui::InputTextWithHint("##newstitle", "What (e.g. Fed Chair speaks)", newsTitle_.data(), newsTitle_.size());
    ImGui::SameLine();
    if (glass::button("Add", kBlue, ImVec2(-1, 0))) {
        int64_t at = 0;
        if (!muuk::ctrader_fix::parseLocalDateTime(newsWhen_.data(), at)) {
            flash("News time must look like 2026-10-02 15:30 (your local time).");
        } else {
            NewsEvent e;
            e.utcMs = at;
            e.title = newsTitle_[0] != '\0' ? std::string(newsTitle_.data()) : std::string("News");
            e.impact = "High";
            e.manual = true;
            quickApply(controller, snap, connected, [e](LiveSettings& x) { x.market.news.push_back(e); });
            newsWhen_[0] = '\0';
            newsTitle_[0] = '\0';
        }
    }
}

void DashboardWindow::renderMarketSettings() {
    MarketHoursConfig& mc = editSettings_.market;
    ImGui::Spacing();
    withFont(boldFont_, [] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Market hours & news");
    });
    ImGui::TextDisabled("When new ranges may open. A running range is always managed: target and stop loss keep "
                        "working through any pause.");
    if (ImGui::Checkbox("Market hours filter", &mc.enabled)) settingsDirty_ = true;
    ImGui::SetItemTooltip("Off: the bot opens ranges at any time the broker has prices (as before).");

    ImGui::BeginDisabled(!mc.enabled);
    if (ImGui::BeginTable("market_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        auto sessionBoxes = [&](const char* label, const char* help, std::array<bool, 4>& flags, const char* idSuffix) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SetItemTooltip("%s", help);
            ImGui::TableNextColumn();
            for (int i = 0; i < muuk::ctrader_fix::kSessionCount; ++i) {
                if (i > 0) ImGui::SameLine();
                const std::string id =
                    std::string(muuk::ctrader_fix::sessionName(static_cast<Session>(i))) + "##" + idSuffix;
                if (ImGui::Checkbox(id.c_str(), &flags[i])) settingsDirty_ = true;
            }
        };
        sessionBoxes("Trade in", "New ranges only while at least one ticked session is open.", mc.tradeIn, "trade");
        sessionBoxes("Pause at the end of",
                     "Around the end of each ticked session (spreads widen, prices jump): no new range.",
                     mc.pauseAtEndOf, "pause");
        settingInt("  minutes before it ends", "Pause starts this long before the session ends.",
                   &mc.pauseMinutesBefore, &settingsDirty_);
        settingInt("  minutes after it ends", "Pause lasts this long after the session ends (and after the daily reopen).",
                   &mc.pauseMinutesAfter, &settingsDirty_);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("News guard (ForexFactory)");
        ImGui::TableNextColumn();
        if (ImGui::Checkbox("On##newsguard", &mc.newsGuard)) settingsDirty_ = true;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        if (ImGui::Checkbox("High##newshigh", &mc.newsHigh)) settingsDirty_ = true;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
        if (ImGui::Checkbox("Medium##newsmed", &mc.newsMedium)) settingsDirty_ = true;
        ImGui::PopStyleColor();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("  news currencies");
        ImGui::SetItemTooltip("Comma separated, e.g. USD or USD, EUR. Gold moves on USD news. ALL = every currency.");
        ImGui::TableNextColumn();
        char cur[64];
        std::snprintf(cur, sizeof(cur), "%s", mc.newsCurrencies.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##newscur", cur, sizeof(cur))) {
            mc.newsCurrencies = cur;
            settingsDirty_ = true;
        }
        settingInt("  minutes before news", "No new range from this long before a guarded release.",
                   &mc.newsMinutesBefore, &settingsDirty_);
        settingInt("  minutes after news", "... until this long after it.", &mc.newsMinutesAfter, &settingsDirty_);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("When a pause starts");
        ImGui::SetItemTooltip("What happens to a range that is still running when a pause begins.");
        ImGui::TableNextColumn();
        const char* closeItems[] = {"Keep managing the running range", "Close it before the weekend",
                                    "Close it at every pause"};
        int closeIndex = mc.closeRangeAt == "weekend" ? 1 : (mc.closeRangeAt == "every" ? 2 : 0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##closeat", &closeIndex, closeItems, 3)) {
            mc.closeRangeAt = closeIndex == 1 ? "weekend" : (closeIndex == 2 ? "every" : "never");
            settingsDirty_ = true;
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    clampSettings(editSettings_);
}

void DashboardWindow::renderSessionStats() {
    if (sessionRowsRev_ != historyRevision_) {
        sessionRowsRev_ = historyRevision_;
        sessionRows_.clear();
        for (const auto& b : history_) {
            std::string name = b.marketSession;
            if (name.empty()) {
                const int64_t ms =
                    muuk::ctrader_fix::localTimestampToUtcMs(b.startedAt.empty() ? b.closedAt : b.startedAt);
                name = ms > 0 ? muuk::ctrader_fix::sessionLabelAt(ms) : "Unknown";
            }
            auto it = std::find_if(sessionRows_.begin(), sessionRows_.end(),
                                   [&](const SessionRow& r) { return r.name == name; });
            if (it == sessionRows_.end()) {
                sessionRows_.push_back(SessionRow{name});
                it = std::prev(sessionRows_.end());
            }
            ++it->baskets;
            (b.net > 0.0 ? it->won : it->lost) += 1;
            it->net += b.net;
            it->commission += b.commission;
        }
        std::sort(sessionRows_.begin(), sessionRows_.end(),
                  [](const SessionRow& a, const SessionRow& b) { return a.baskets > b.baskets; });
    }
    if (sessionRows_.empty()) return;
    ImGui::Spacing();
    withFont(boldFont_, [] { ImGui::TextUnformatted("By trading session"); });
    ImGui::SameLine();
    ImGui::TextDisabled("(the session(s) open when each basket started)");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("sessions", 7, flags)) {
        ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Baskets");
        ImGui::TableSetupColumn("Won / lost");
        ImGui::TableSetupColumn("Win rate");
        ImGui::TableSetupColumn("Net");
        ImGui::TableSetupColumn("Avg / basket");
        ImGui::TableSetupColumn("Commission");
        ImGui::TableHeadersRow();
        for (const auto& r : sessionRows_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.baskets);
            ImGui::TableNextColumn();
            ImGui::TextColored(kGreen, "%d", r.won);
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextColored(r.lost > 0 ? kRed : kGrey, "%d", r.lost);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", r.baskets > 0 ? 100.0 * r.won / r.baskets : 0.0);
            ImGui::TableNextColumn();
            ImGui::TextColored(pnlColor(r.net), "%s", money(r.net).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(pnlColor(r.net), "%s", money(r.baskets > 0 ? r.net / r.baskets : 0.0).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", money(-r.commission).c_str());
        }
        ImGui::EndTable();
    }
}

void DashboardWindow::renderTargetBar(LiveController& controller, const LiveSnapshot& snap, bool connected) {
    const LiveSettings& s = snap.settings;
    const double net = floatingPnl(snap);
    const double target = snap.targetNow > 0.0 ? snap.targetNow : s.profitTarget;
    const bool haveStop = s.stopLoss > 0.0;
    const double lossScale = haveStop ? s.stopLoss : (s.maxBasketLoss > 0.0 ? s.maxBasketLoss : std::max(target, 0.01));
    const float h = ImGui::GetFrameHeight();

    // Row 1: gear, net, target.
    if (gearButton("##targetgear", h)) {
        ImGui::OpenPopup("target_popup");
        // Open below the gear if it fits, otherwise above it -- never off the bottom of the window.
        const ImVec2 gearMin = ImGui::GetItemRectMin();
        const ImVec2 gearMax = ImGui::GetItemRectMax();
        const float displayH = ImGui::GetIO().DisplaySize.y;
        const bool below = displayH - gearMax.y > 300.0f;
        // Applied right before BeginPopup (a tooltip in between would take it otherwise).
        targetPopupPos_ = below ? ImVec2(gearMin.x, gearMax.y + 4.0f) : ImVec2(gearMin.x, gearMin.y - 4.0f);
        targetPopupPivot_ = below ? ImVec2(0.0f, 0.0f) : ImVec2(0.0f, 1.0f);
    }
    ImGui::SetItemTooltip("Profit target: fixed, or automatic from what the open positions cost.");
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("NET");
    ImGui::SameLine();
    withFont(boldFont_, [&] { ImGui::TextColored(pnlColor(net), "%s", money(net).c_str()); });
    const std::string targetText =
        "target " + money(target) +
        (snap.escapeActive ? "  (break-even escape, level " + std::to_string(snap.filledLevel) + ")"
         : s.autoTarget    ? "  (level " + std::to_string(targetLevel(s.autoTargetPercent)) + " = " +
                              std::to_string(s.autoTargetPercent) + "%)"
                           : "");
    ImGui::SameLine(0.0f, 12.0f);
    const float targetW = ImGui::CalcTextSize(targetText.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > targetW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - targetW);
    ImGui::TextColored(kGreen, "%s", targetText.c_str());

    // Row 2: the bar.
    glass::pnlBar("net_bar", static_cast<float>(net), static_cast<float>(lossScale),
                  static_cast<float>(std::max(target, 0.01)));
    ImGui::SetItemTooltip("Net P/L of the open positions (after commission and spread).\n"
                          "Right, green: towards the profit target. Left, red: towards %s.\n"
                          "The colour deepens as it grows; each move pulses green (up) or red (down).",
                          haveStop ? "the stop loss" : (s.maxBasketLoss > 0.0 ? "the max basket loss" : "a loss the size of the target"));

    // Row 3: the scale under it.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize() * 0.82f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const std::string lossText = money(-lossScale) + (haveStop ? " stop" : "");
        const std::string winText = money(target);
        const float winW = font->CalcTextSizeA(size, FLT_MAX, 0.0f, winText.c_str()).x;
        const float zeroW = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "0").x;
        dl->AddText(font, size, ImVec2(p.x, p.y), ImGui::GetColorU32(kRed), lossText.c_str());
        dl->AddText(font, size, ImVec2(p.x + (w - zeroW) * 0.5f, p.y), ImGui::GetColorU32(glass::palette().textDim), "0");
        dl->AddText(font, size, ImVec2(p.x + w - winW, p.y), ImGui::GetColorU32(kGreen), winText.c_str());
        ImGui::Dummy(ImVec2(w, size + 2.0f));
    }

    ImGui::SetNextWindowPos(targetPopupPos_, ImGuiCond_Appearing, targetPopupPivot_);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(FLT_MAX, std::max(200.0f, ImGui::GetIO().DisplaySize.y - 24.0f)));
    if (ImGui::BeginPopup("target_popup")) {
        withFont(boldFont_, [] { ImGui::TextUnformatted("Profit target"); });
        const std::optional<LiveSnapshot> opt(snap);
        bool autoOn = s.autoTarget;
        if (ImGui::Checkbox("Auto - from what the open positions cost", &autoOn)) {
            quickApply(controller, opt, connected, [autoOn](LiveSettings& x) { x.autoTarget = autoOn; });
        }
        ImGui::TextDisabled("Target = a multiple of the cost (commission open + close + spread) of every open\n"
                            "position: small for one 0.01 lot, bigger with every fill.\n"
                            "Level 20 = 200%% (twice the cost), 40 = 240%%, 60 = 280%%, 80 = 320%%, 100 = 360%%.");
        ImGui::BeginDisabled(!autoOn);
        for (int level : {20, 40, 60, 80, 100}) {
            if (level != 20) ImGui::SameLine();
            const std::string label = std::to_string(level);
            const int p = levelPercent(level);
            if (ImGui::RadioButton(label.c_str(), targetLevel(s.autoTargetPercent) == level)) {
                quickApply(controller, opt, connected, [p](LiveSettings& x) { x.autoTargetPercent = p; });
            }
            ImGui::SetItemTooltip("Level %d: target = %d%% of the cost", level, p);
        }
        ImGui::EndDisabled();
        const double cost = snap.live.commission + 2.0 * snap.live.spread;
        ImGui::Separator();
        ImGui::Text("Cost of the open positions now: %s", money(cost).c_str());
        ImGui::TextDisabled("  commission %s + spread %s", money(snap.live.commission).c_str(),
                            money(2.0 * snap.live.spread).c_str());
        if (autoOn) {
            ImGui::Text("Target now: %s  (level %d = %d%% of the cost%s)", money(target).c_str(),
                        targetLevel(s.autoTargetPercent), s.autoTargetPercent,
                        snap.positions.empty() ? ", set by the first fill" : "");
        } else {
            ImGui::Text("Fixed target: %s  (Settings > Profit target)", money(s.profitTarget).c_str());
        }
        ImGui::TextDisabled("Applies at once, also to the running basket. Saved to your config.");
        ImGui::EndPopup();
    }
}

void DashboardWindow::renderAuditTab(const std::optional<LiveSnapshot>& snap, bool connected) {
    ImGui::BeginChild("audit_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    withFont(boldFont_, [] { ImGui::TextUnformatted("Audit"); });
    ImGui::SameLine();
    ImGui::TextDisabled("-- the bot checks itself every second and fixes what it can by itself");
    if (!connected || !snap || snap->audit.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Connect to run the audit.");
        ImGui::EndChild();
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_PadOuterX;
    if (ImGui::BeginTable("audit", 3, flags)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (const auto& c : snap->audit) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec4 color = c.level == 0 ? kGreen : (c.level >= 2 ? kRed : kAmber);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 8.0f, p.y + h * 0.5f + 1.0f), 5.5f,
                                                        ImGui::GetColorU32(color), 14);
            ImGui::Dummy(ImVec2(16.0f, h));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            if (c.level == 0) {
                ImGui::TextDisabled("%s", c.detail.c_str());
            } else {
                ImGui::TextColored(color, "%s", c.detail.c_str());
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    withFont(boldFont_, [] { ImGui::TextUnformatted("What it found and did"); });
    if (snap->auditLog.empty()) {
        ImGui::TextDisabled("Nothing so far.");
    } else {
        for (auto it = snap->auditLog.rbegin(); it != snap->auditLog.rend(); ++it) {
            ImGui::TextDisabled("%s", it->time.c_str());
            ImGui::SameLine();
            const ImVec4 color = it->level >= 2 ? kRed : (it->level == 1 ? kAmber : glass::palette().text);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextWrapped("%s", it->text.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    withFont(boldFont_, [] { ImGui::TextUnformatted("What each check does by itself"); });
    ImGui::TextWrapped(
        "Prices: no price for 2 min while the market is open = a dead feed; the bot reconnects and carries on with "
        "the saved basket.\n"
        "Positions: every 30 s the broker's own position and order lists are compared with the bot's. A missing "
        "pending order is placed again; a position closed outside the bot stops the range safely; a position the bot "
        "didn't open is listed.\n"
        "Pending order: a running range always has its next order; if it's missing it's placed again (3 tries).\n"
        "Closing orders: a close that isn't confirmed within 10 s, or is rejected, is checked with the broker -- "
        "already closed there: booked; still open: closed again (3 tries), then it waits for you.\n"
        "Target / stop loss: if the net passed the target or stop loss and the basket is still open, it's closed.\n"
        "Bot vs positions: warns when the range's own lot count differs from its positions (a hand close or a "
        "partial fill).\n"
        "Daily loss limit: closes everything and pauses until tomorrow (Settings).\n"
        "Risk settings: warns while there is no stop loss / no daily loss limit.");
    ImGui::EndChild();
}

void DashboardWindow::renderRiskCalculator(const std::optional<LiveSnapshot>& snap) {
    LiveSettings& s = editSettings_;
    ImGui::Spacing();
    withFont(boldFont_, [] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Risk & capital");
    });
    ImGui::TextDisabled("The worst case: the price bounces between the two lines, so every fill adds the next lot "
                        "on the other side.");

    const bool fromMt5 = snap && snap->brokerAccountFresh;
    if (fromMt5) {
        const auto& a = snap->brokerAccount;
        ImGui::TextColored(kGreen, "From MT5 (%s): balance %s, equity %s, margin %s, free %s, 1:%.0f%s", a.login.c_str(),
                           money(a.balance).c_str(), money(a.equity).c_str(), money(a.margin).c_str(),
                           money(a.freeMargin).c_str(), a.leverage, a.cent() ? " -- cent account, amounts in US cents" : "");
        ImGui::TextDisabled("The table and the guards use these; the balance and leverage below are only for cTrader.");
    }
    if (ImGui::BeginTable("risk_settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        settingDouble("Account balance ($)",
                      "Your balance -- FIX doesn't send it. Used for this table and the margin warning in the Audit.",
                      &s.accountBalance, 100.0, "%.2f", &settingsDirty_);
        settingDouble("Leverage (1:x)", "Your account's leverage, e.g. 500 for 1:500. Sets the margin per lot.",
                      &s.leverage, 50.0, "%.0f", &settingsDirty_);
        settingInt("Break-even escape from level",
                   "From this fill on, the target drops to the escape amount below: a deep basket closes at the "
                   "first bounce back instead of waiting for the full target. 0 = off.",
                   &s.escapeFromLevel, &settingsDirty_);
        settingDouble("  escape at net ($)", "Net after every cost at which an escaping basket closes.",
                      &s.escapeProfit, 0.25, "%.2f", &settingsDirty_);
        settingDouble("Lock & pause at loss ($)",
                      "When the basket's net reaches minus this: pending orders are cancelled and one opposite order "
                      "freezes the loss (a hedge). The bot then waits for you -- the loss doesn't go away, and swap "
                      "is charged every night. 0 = off.",
                      &s.lockAtLoss, 10.0, "%.2f", &settingsDirty_);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Account type");
        ImGui::SetItemTooltip("Cent: balance and P/L are in US cents (USC) -- 100 USC = $1. Every $ amount on this "
                              "page is then in USC. Each account file keeps its own settings, so a cent account "
                              "simply gets its own file.");
        ImGui::TableNextColumn();
        {
            int kind = s.accountKind == "cent" ? 1 : 0;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo("##accountKind", &kind, "Standard ($)\0Cent (USC)\0")) {
                s.accountKind = kind == 1 ? "cent" : "standard";
                settingsDirty_ = true;
            }
        }
        settingDouble("Guard: hold the next level under (%)",
                      "Margin level (equity / margin) the next fill would leave. Under this, that level is not "
                      "placed: a stop order for the net lots (a lock) waits at its line instead, and the basket "
                      "closes at the escape amount if the price turns back first. Brokers call a margin call at "
                      "100 % and stop out at 50 % or less. 0 = off. Needs the balance.",
                      &s.recovery.guardLockMarginLevel, 50.0, "%.0f", &settingsDirty_);
        settingDouble("Guard: lock at once under (%)",
                      "Margin level now. Under this the basket is locked at market right away. 0 = off.",
                      &s.recovery.guardStopMarginLevel, 50.0, "%.0f", &settingsDirty_);
        settingDouble("Hedged margin (%)",
                      "How the broker charges a buy + sell pair. cTrader (IC Markets): 100 = the larger side only. "
                      "MT5 brokers say it per symbol (RoboForex ProCent gold: 50).",
                      &s.recovery.hedgedMarginPercent, 10.0, "%.0f", &settingsDirty_);
        settingCheck("Close at break-even when guarded",
                     "While a lock stop waits or a lock holds: once the net is back at the escape amount, close "
                     "everything.",
                     &s.recovery.closeAtBreakEven, &settingsDirty_);
        settingCheck("Escape from a lock",
                     "Off by default. From a lock: stop orders outside the range, sized so a run of the escape move "
                     "wins the frozen loss back. If the price turns back (into the range, or to the entry once it ran "
                     "the trail distance) it locks again. It's a bet on a breakout -- each failed try adds its cost.",
                     &s.recovery.escapeEnabled, &settingsDirty_);
        if (s.recovery.escapeEnabled) {
            settingDouble("  escape move ($)", "Price run the escape trade is sized to win the loss back over.",
                          &s.recovery.escapeMove, 0.5, "%.2f", &settingsDirty_);
            settingDouble("  beyond the range by ($)", "Escape stop orders go this far outside the range lines.",
                          &s.recovery.escapeBuffer, 0.1, "%.2f", &settingsDirty_);
            settingDouble("  trail to entry after ($)",
                          "Once the escape ran this far, it locks again as soon as the price is back at its entry.",
                          &s.recovery.escapeTrailAt, 0.25, "%.2f", &settingsDirty_);
            settingInt("  tries per basket", "After this many escapes that turned back, the basket just stays locked.",
                       &s.recovery.escapeMaxTries, &settingsDirty_);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Lot preset");
        ImGui::SetItemTooltip("Aggressive: x2 each fill. Balanced: x1.5. Safe: +0.01 each fill. The table shows "
                              "what each can lose.");
        ImGui::TableNextColumn();
        const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        auto preset = [&](const char* label, double mult, ImVec4 color) {
            const bool on = std::fabs(s.lotMultiplier - mult) < 0.01 && (mult > 1.0 || std::fabs(s.lotIncrement - 0.01) < 0.001);
            if (glass::button(label, on ? color : glass::palette().neutral, ImVec2(third, 0))) {
                s.lotMultiplier = mult;
                if (mult <= 1.0) s.lotIncrement = 0.01;
                settingsDirty_ = true;
            }
        };
        preset("Aggressive x2", 2.0, kRed);
        ImGui::SameLine();
        preset("Balanced x1.5", 1.5, kAmber);
        ImGui::SameLine();
        preset("Safe +0.01", 1.0, kGreen);
        ImGui::EndTable();
    }
    clampSettings(s);

    // The table.
    muuk::core::RiskTableInput in;
    const muuk::core::SymbolSpec spec;
    in.spec = spec;
    in.lots = muuk::core::LotProgressionConfig{muuk::core::Volume::fromDouble(s.initialLot, spec),
                                               muuk::core::Volume::fromDouble(s.lotIncrement, spec), s.lotMultiplier};
    in.costs.commissionPerLotRoundTurn = s.commissionPerLot;
    in.costs.commissionPerMillionPerSide = s.commissionPerMillion;
    in.rangeWidth = s.rangeWidth;
    const bool live = snap && snap->bid && snap->ask;
    in.price = live ? (*snap->bid + *snap->ask) / 2.0 : 4270.0;
    in.spread = live ? std::max(0.0, *snap->ask - *snap->bid) : 0.30;
    in.leverage = s.leverage;
    in.maxLot = s.maxLot;
    in.levels = std::clamp(s.maxOrders, 1, 16);
    in.balance = s.accountBalance;
    in.hedgedPercent = s.recovery.hedgedMarginPercent;
    if (fromMt5) {
        // In the account's own money (cents on a cent account), at MT5's margin per lot.
        in.spec.contractSize = snap->moneyPerPointPerLot;
        in.spec.tickValue = snap->moneyPerPointPerLot * in.spec.tickSize;
        if (snap->marginLeverage > 0.0) in.leverage = snap->marginLeverage;
        in.balance = snap->brokerAccount.balance;
        if (snap->brokerAccount.hedgedMarginPercent >= 0.0) in.hedgedPercent = snap->brokerAccount.hedgedMarginPercent;
    }
    const auto rows = muuk::core::riskTable(in);
    const double lockGuard = s.recovery.guardLockMarginLevel;
    const double stopGuard = s.recovery.guardStopMarginLevel;
    int guardLevel = 0;  // the level the margin guard holds back (0 = none in the table)
    if (s.accountBalance > 0.0 && lockGuard > 0.0) {
        for (const auto& r : rows) {
            if (r.marginLevel < lockGuard) {
                guardLevel = r.level;
                break;
            }
        }
    }

    const double balance = in.balance;
    int lastSafe = 0;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_PadOuterX;
    ImGui::TextDisabled("At %s%.2f, spread %.2f, 1:%.0f, range $%.2f:", live ? "" : "e.g. ", in.price, in.spread,
                        in.leverage, in.rangeWidth);
    if (ImGui::BeginTable("risk_table", 9, flags)) {
        ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Lot");
        ImGui::TableSetupColumn("Total lots");
        ImGui::TableSetupColumn("Worst loss");
        ImGui::TableSetupColumn("Margin");
        ImGui::TableSetupColumn("Margin level");
        ImGui::TableSetupColumn("Equity left");
        ImGui::TableSetupColumn("Recovery move");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        bool lockShown = false;
        for (const auto& r : rows) {
            const double equity = balance - r.worstLoss;
            const double marginLevel = r.margin > 0.0 ? equity / r.margin : 99.0;
            const bool stopOut = balance > 0.0 && (equity <= 0.0 || marginLevel < 0.5);
            const bool marginCall = balance > 0.0 && !stopOut && marginLevel < 1.0;
            if (balance > 0.0 && !stopOut && !marginCall) lastSafe = r.level;
            const ImVec4 rowColor = stopOut ? kRed : (marginCall ? kAmber : glass::palette().text);
            ImGui::TableNextRow();
            if (stopOut) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(withAlpha(kRed, 0.10f)));
            ImGui::PushStyleColor(ImGuiCol_Text, rowColor);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.level);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", r.lot);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", r.totalLots);
            ImGui::TableNextColumn();
            ImGui::Text("%s", money(-r.worstLoss).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("$%.0f", r.margin);
            ImGui::TableNextColumn();
            if (balance > 0.0) {
                ImGui::TextColored(marginLevelColor(r.marginLevel, lockGuard, stopGuard), "%.0f%%",
                                   std::max(0.0, std::min(r.marginLevel, 99999.0)));
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableNextColumn();
            if (balance > 0.0) {
                ImGui::Text("%s", money(equity).c_str());
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableNextColumn();
            if (r.recoveryMove > 0.0) {
                ImGui::Text("$%.1f", r.recoveryMove);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            std::string note;
            if (stopOut) note = "stop-out";
            else if (marginCall) note = "margin call";
            if (s.escapeFromLevel > 0 && r.level >= s.escapeFromLevel) note += std::string(note.empty() ? "" : ", ") + "escape";
            if (guardLevel > 0 && r.level == guardLevel) {
                note = "guard: lock stop";
            } else if (guardLevel > 0 && r.level > guardLevel) {
                note = "not reached";
            }
            if (s.lockAtLoss > 0.0 && !lockShown && r.worstLoss >= s.lockAtLoss) {
                note += std::string(note.empty() ? "" : ", ") + "lock here";
                lockShown = true;
            }
            ImGui::TextColored(stopOut ? kRed : (marginCall ? kAmber : kBlue), "%s", note.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SetItemTooltip("Worst loss: floating at that fill with commission and spread. Recovery move: how far the "
                          "price must then run past the last fill, without coming back to the other line, for the "
                          "basket to be back at 0.");
    if (balance <= 0.0) {
        ImGui::TextColored(kAmber, "Enter your balance above to see where margin and balance run out.");
    } else if (!rows.empty()) {
        ImGui::TextColored(lastSafe >= static_cast<int>(rows.size()) ? kGreen : kAmber,
                           "With $%.0f at 1:%.0f the account holds %d level(s) of bouncing%s.", balance,
                           in.leverage, lastSafe,
                           lastSafe >= static_cast<int>(rows.size()) ? " -- the whole table" : "; after that, margin call / stop-out");
        if (guardLevel > 0) {
            const auto& g = rows[static_cast<std::size_t>(guardLevel - 1)];
            ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
            ImGui::TextWrapped("Margin guard: level %d (margin level about %.0f%%) is never placed -- the basket is "
                               "locked there with %.2f lot instead, frozen near %s.",
                               guardLevel, std::max(0.0, g.marginLevel), std::fabs(g.netLots - (guardLevel % 2 == 1 ? g.lot : -g.lot)),
                               money(-g.worstLoss).c_str());
            ImGui::PopStyleColor();
        }
    }

    renderReverseCalculator(snap, rows);
}

void DashboardWindow::renderReverseCalculator(const std::optional<LiveSnapshot>& snap,
                                              const std::vector<muuk::core::RiskRow>& rows) {
    const LiveSettings& s = editSettings_;
    ImGui::Spacing();
    withFont(boldFont_, [] {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Reverse trade calculator");
    });
    ImGui::PushStyleColor(ImGuiCol_Text, glass::palette().textDim);
    ImGui::TextWrapped("The opposite trade for a basket in loss: a lock (the loss stops moving), or enough lots to "
                       "be back at 0 after a move.");
    ImGui::PopStyleColor();

    // Which book: the live one, or the worst case at a level of the table.
    const bool haveLive = snap && snap->bid && snap->ask && !snap->positions.empty();
    double buys = 0.0;
    double sells = 0.0;
    double floating = 0.0;
    double equity = -1.0;
    const bool live = snap && snap->bid && snap->ask;
    const double price = live ? (*snap->bid + *snap->ask) / 2.0 : 4270.0;
    const double spread = live ? std::max(0.0, *snap->ask - *snap->bid) : 0.30;
    if (ImGui::BeginTable("rev_inputs", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Basket");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string preview = reverseLevel_ == 0 ? std::string(haveLive ? "the open positions now" : "the open positions (none)")
                                                 : "worst case at level " + std::to_string(reverseLevel_);
        if (ImGui::BeginCombo("##revbasket", preview.c_str())) {
            if (ImGui::Selectable("the open positions now", reverseLevel_ == 0)) reverseLevel_ = 0;
            for (const auto& r : rows) {
                const std::string label = "worst case at level " + std::to_string(r.level);
                if (ImGui::Selectable(label.c_str(), reverseLevel_ == r.level)) reverseLevel_ = r.level;
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Back to 0 after a move of ($)");
        ImGui::SetItemTooltip("0 = just lock (the net lots). More: that many extra lots that win the loss back if the "
                              "price moves this far in the new trade's direction.");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputDouble("##revmove", &reverseMove_, 0.5, 2.0, "%.2f");
        reverseMove_ = std::clamp(reverseMove_, 0.0, 100.0);
        ImGui::EndTable();
    }

    if (reverseLevel_ > 0 && reverseLevel_ <= static_cast<int>(rows.size())) {
        const auto& r = rows[static_cast<std::size_t>(reverseLevel_ - 1)];
        buys = (r.totalLots + r.netLots) / 2.0;
        sells = (r.totalLots - r.netLots) / 2.0;
        floating = -r.worstLoss;
        const double bal = snap && snap->brokerAccountFresh ? snap->brokerAccount.balance : s.accountBalance;
        if (bal > 0.0) equity = bal - r.worstLoss;
    } else if (haveLive) {
        for (const auto& p : snap->positions) (p.side == OrderSide::BuyStop ? buys : sells) += p.lots;
        floating = snap->live.net;
        equity = snap->equityNow;
    } else {
        ImGui::TextDisabled("No open positions -- pick a level of the table above to try it.");
        return;
    }

    muuk::core::ReverseTradeInput in;
    in.spec = muuk::core::SymbolSpec{};
    in.costs.commissionPerLotRoundTurn = s.commissionPerLot;
    in.costs.commissionPerMillionPerSide = s.commissionPerMillion;
    in.margin = muuk::core::MarginRule{std::max(1.0, s.leverage), s.recovery.hedgedMarginPercent};
    if (snap && snap->brokerAccountFresh) {
        in.spec.contractSize = snap->moneyPerPointPerLot;
        in.spec.tickValue = snap->moneyPerPointPerLot * in.spec.tickSize;
        if (snap->marginLeverage > 0.0) in.margin.leverage = snap->marginLeverage;
        if (snap->brokerAccount.hedgedMarginPercent >= 0.0) in.margin.hedgedPercent = snap->brokerAccount.hedgedMarginPercent;
    }
    in.buyLots = buys;
    in.sellLots = sells;
    in.floating = floating;
    in.equity = equity < 0.0 ? 0.0 : equity;
    in.price = price;
    in.spread = spread;
    in.move = reverseMove_;
    in.maxLot = s.maxLot;
    const muuk::core::ReverseTrade t = muuk::core::reverseTrade(in);
    const double net = buys - sells;

    ImGui::Text("Book: buy %.2f / sell %.2f lot (net %+.2f), floating %s", buys, sells, net, money(floating).c_str());
    if (t.lot <= 0.0) {
        ImGui::TextColored(kGreen, "Already balanced: buys = sells, the loss can't move.");
        return;
    }
    ImGui::TextColored(t.buy ? kGreen : kRed, "%s %.2f lot at market%s", t.buy ? "BUY" : "SELL", t.lot,
                       t.capped ? " (capped at the max lot)" : "");
    ImGui::Text("Its cost (commission + spread): %s", money(-t.cost).c_str());
    if (t.lockOnly) {
        ImGui::TextWrapped("After it: the loss is frozen at about %s wherever the price goes.", money(t.lossAfter).c_str());
    } else {
        const double over = t.lot - std::fabs(net);
        ImGui::TextWrapped("After it: net %s %.2f lot. A $%.2f move %s brings the basket back to about 0; each $1 "
                           "the other way adds %s to the loss.",
                    t.buy ? "long" : "short", over, reverseMove_, t.buy ? "up" : "down",
                    money(-over * 100.0).c_str());
    }
    if (equity < 0.0) {
        ImGui::TextColored(kAmber, "Margin after: $%.2f (enter the balance for free margin and margin level).",
                           t.marginAfter);
    } else {
        ImGui::TextColored(marginLevelColor(t.marginLevelAfter, s.recovery.guardLockMarginLevel,
                                            s.recovery.guardStopMarginLevel),
                           "Margin after: $%.2f, free margin %s, margin level %.0f%%%s", t.marginAfter,
                           money(t.freeMarginAfter).c_str(), std::max(0.0, std::min(t.marginLevelAfter, 99999.0)),
                           t.marginLevelAfter < 100.0 ? " -- not affordable (margin call)" : "");
    }
}

}  // namespace muuk::desktop
