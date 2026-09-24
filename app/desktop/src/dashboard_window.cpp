#include "dashboard_window.hpp"

#include <algorithm>
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
#include "muuk/ctrader_fix/basket_history.hpp"

namespace muuk::desktop {

using muuk::core::EngineStatus;
using muuk::core::OrderSide;
using muuk::core::StartSide;
using muuk::ctrader_fix::BasketRecord;
using muuk::ctrader_fix::LatencySummary;
using muuk::ctrader_fix::LiveSettings;
using muuk::ctrader_fix::LiveSnapshot;
using muuk::ctrader_fix::VolumeBucket;
using muuk::ctrader_fix::VolumePeriod;
using glass::kAmber;
using glass::kBlue;
using glass::kGreen;
using glass::kGrey;
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

std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%.2f", v < -0.005 ? "-" : (v > 0.005 ? "+" : ""), std::fabs(v));
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

// The range as a horizontal price scale: the band between the two
// boundaries, pending orders as triangles (Buy Stop above the track, Sell
// Stop below), open positions as dots, and the live mid as a marker.
void drawRangeChart(const LiveSnapshot& snap, float height) {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glass::Palette& pal = glass::palette();

    double lo = snap.rangeLower;
    double hi = snap.rangeUpper;
    auto include = [&](double p) {
        if (p <= 0.0) return;
        lo = std::min(lo, p);
        hi = std::max(hi, p);
    };
    if (snap.bid) include(*snap.bid);
    if (snap.ask) include(*snap.ask);
    for (const auto& p : snap.positions) include(p.entryPrice);
    for (const auto& o : snap.pendingOrders) include(o.price);
    if (hi - lo < 0.5) {
        const double mid = (hi + lo) / 2.0;
        lo = mid - 0.25;
        hi = mid + 0.25;
    }
    const double pad = (hi - lo) * 0.12;
    lo -= pad;
    hi += pad;

    const float textH = ImGui::GetTextLineHeight();
    const float left = origin.x + 6.0f;
    const float right = origin.x + width - 6.0f;
    const float trackY = origin.y + textH + 34.0f;
    auto xOf = [&](double p) { return left + static_cast<float>((p - lo) / (hi - lo)) * (right - left); };

    dl->AddRectFilled(ImVec2(left, trackY - 2), ImVec2(right, trackY + 2),
                      ImGui::GetColorU32(glass::isDark() ? ImVec4(1, 1, 1, 0.08f) : ImVec4(0, 0, 0, 0.08f)), 2.0f);

    if (snap.rangeUpper > snap.rangeLower) {
        const float x0 = xOf(snap.rangeLower);
        const float x1 = xOf(snap.rangeUpper);
        dl->AddRectFilledMultiColor(ImVec2(x0, trackY - 12), ImVec2(x1, trackY + 12),
                                    ImGui::GetColorU32(withAlpha(kRed, 0.16f)), ImGui::GetColorU32(withAlpha(kGreen, 0.16f)),
                                    ImGui::GetColorU32(withAlpha(kGreen, 0.16f)), ImGui::GetColorU32(withAlpha(kRed, 0.16f)));
        dl->AddLine(ImVec2(x0, trackY - 16), ImVec2(x0, trackY + 16), ImGui::GetColorU32(kRed), 2.0f);
        dl->AddLine(ImVec2(x1, trackY - 16), ImVec2(x1, trackY + 16), ImGui::GetColorU32(kGreen), 2.0f);
        const std::string loText = price(snap.rangeLower);
        const std::string hiText = price(snap.rangeUpper);
        const float labelY = trackY + 30.0f;
        const ImU32 dim = ImGui::GetColorU32(pal.textDim);
        const float loW = ImGui::CalcTextSize(loText.c_str()).x;
        const float hiW = ImGui::CalcTextSize(hiText.c_str()).x;
        float loX = x0 - loW / 2;
        float hiX = x1 - hiW / 2;
        if (loX + loW + 8.0f > hiX) {  // a narrow range on this scale: put them either side instead
            loX = x0 - loW - 4.0f;
            hiX = x1 + 4.0f;
        }
        dl->AddText(ImVec2(std::max(left, loX), labelY), dim, loText.c_str());
        dl->AddText(ImVec2(std::min(right - hiW, hiX), labelY), dim, hiText.c_str());
    }

    for (const auto& o : snap.pendingOrders) {
        const float x = xOf(o.price);
        if (o.side == OrderSide::BuyStop) {
            dl->AddTriangleFilled(ImVec2(x, trackY - 29), ImVec2(x - 7, trackY - 18), ImVec2(x + 7, trackY - 18),
                                  ImGui::GetColorU32(kGreen));
        } else {
            dl->AddTriangleFilled(ImVec2(x, trackY + 29), ImVec2(x + 7, trackY + 18), ImVec2(x - 7, trackY + 18),
                                  ImGui::GetColorU32(kRed));
        }
    }
    for (const auto& p : snap.positions) {
        const float x = xOf(p.entryPrice);
        const ImVec4 c = p.side == OrderSide::BuyStop ? kGreen : kRed;
        dl->AddCircleFilled(ImVec2(x, trackY), 9.0f, ImGui::GetColorU32(withAlpha(c, 0.22f)), 20);
        dl->AddCircleFilled(ImVec2(x, trackY), 5.0f, ImGui::GetColorU32(c), 16);
    }

    if (snap.bid && snap.ask) {
        const double mid = (*snap.bid + *snap.ask) / 2.0;
        const float x = xOf(mid);
        const ImU32 textCol = ImGui::GetColorU32(pal.text);
        dl->AddLine(ImVec2(x, origin.y + textH + 3), ImVec2(x, trackY + 12), textCol, 1.5f);
        dl->AddCircleFilled(ImVec2(x, trackY), 3.0f, textCol, 12);
        const std::string midText = price(mid);
        const float tw = ImGui::CalcTextSize(midText.c_str()).x;
        const float tx = std::clamp(x - tw / 2, left, right - tw);
        dl->AddText(ImVec2(tx, origin.y), textCol, midText.c_str());
    }
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
    controller.setAutoReconnect(prefs_.autoReconnect);
    controller.tick();  // reconnects a dropped connection when it's time
    const bool running = controller.isRunning();
    const std::optional<LiveSnapshot> snap = controller.snapshot();
    samplePerformance(snap);
    const bool connected = running && snap && snap->connected;

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
        if (!running && std::filesystem::is_regular_file(configPath, ec)) {
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
    if (ImGui::BeginTabBar("tabs")) {
        const std::string posLabel =
            "  Positions  " + std::to_string(snap ? snap->positions.size() : 0) + "  ###positions";
        if (ImGui::BeginTabItem(posLabel.c_str(), nullptr, tabFlags(Tab::Positions))) {
            if (snap) {
                renderPositionsTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            ImGui::EndTabItem();
        }
        const std::string ordLabel =
            "  Pending orders  " + std::to_string(snap ? snap->pendingOrders.size() : 0) + "  ###orders";
        if (ImGui::BeginTabItem(ordLabel.c_str(), nullptr, tabFlags(Tab::Orders))) {
            if (snap) {
                renderOrdersTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            ImGui::EndTabItem();
        }
        const std::string histLabel =
            "  History  " + std::to_string(history_.size()) + "  ###history";
        if (ImGui::BeginTabItem(histLabel.c_str(), nullptr, tabFlags(Tab::History))) {
            renderHistoryTab(controller, snap);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("  Volume  ###volume", nullptr, tabFlags(Tab::Volume))) {
            renderVolumeTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("  Performance  ###performance", nullptr, tabFlags(Tab::Performance))) {
            renderPerformanceTab(controller, snap);
            ImGui::EndTabItem();
        }
        const std::string settingsLabel = std::string("  Settings") + (settingsDirty_ ? " *" : "") + "  ###settings";
        if (ImGui::BeginTabItem(settingsLabel.c_str(), nullptr, tabFlags(Tab::Settings))) {
            renderSettingsPanel(controller, snap, connected);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("  Log  ###log", nullptr, tabFlags(Tab::Log))) {
            renderLogTab(controller);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        selectTab_ = Tab::None;
    }
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
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("XAUUSD range bot  |  IC Markets FIX");
    ImGui::SameLine(0.0f, 20.0f);
    if (account_.loaded) {
        glass::pill(account_.live ? "LIVE MONEY" : "DEMO", account_.live ? kRed : kBlue);
        ImGui::SetItemTooltip("Account %s (%s) - change it in Settings > Account.", account_.traderLogin.data(),
                              account_.live ? "live, real money" : "demo");
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
    glass::endCard();
}

int DashboardWindow::tabFlags(Tab tab) const {
    return selectTab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
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

    const float chartHeight = ImGui::GetTextLineHeight() * 2.0f + 72.0f;
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
                         "Your configured round-turn commission x lots. Set it in Strategy settings to match your broker.",
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
        if (snap->settings.commissionPerLot <= 0.0) {
            ImGui::TextColored(kAmber, "Commission is set to $0 - enter your broker's rate in Strategy settings.");
        }
    }

    if (snap && connected && snap->settings.profitTarget > 0.0) {
        const double net = floatingPnl(*snap);
        const double lossScale =
            snap->settings.maxBasketLoss > 0.0 ? snap->settings.maxBasketLoss : snap->settings.profitTarget;
        const std::string overlay = money(net) + " net   |   target " + money(snap->settings.profitTarget);
        glass::balanceBar("target_bar", static_cast<float>(net), static_cast<float>(lossScale),
                          static_cast<float>(snap->settings.profitTarget), overlay.c_str());
        ImGui::SetItemTooltip("Green grows to the right towards the profit target; red grows to the left while the "
                              "basket is in loss (full at %s).",
                              snap->settings.maxBasketLoss > 0.0 ? "your max basket loss" : "the same size as the target");
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
    ImGui::TextDisabled("Saved to your config file, so the app always opens with them. They apply from the next "
                        "range - never to a running one.");

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
        settingDouble("Profit target ($)", "Floating net profit at which the whole basket is booked and closed.",
                      &s.profitTarget, 0.25, "%.2f", &settingsDirty_);
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
        settingDouble("Commission ($/lot)",
                      "Round-turn commission (open + close) per 1.00 lot, as your broker charges it on XAUUSD. "
                      "Deducted from net P/L and counted before the profit target is reached.",
                      &s.commissionPerLot, 0.50, "%.2f", &settingsDirty_);
        clampSettings(s);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    renderLotPreview(editSettings_);

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

    ImGui::EndChild();
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
    if (ImGui::BeginTable("positions", 10, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Lots");
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("Slippage");
        ImGui::TableSetupColumn("Now");
        ImGui::TableSetupColumn("Gross");
        ImGui::TableSetupColumn("Costs");
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
            ImGui::TextColored(pnlColor(p.pnl.gross), "%s", money(p.pnl.gross).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(p.pnl.costs() > 0.005 ? kAmber : glass::palette().textDim, "-$%.2f", p.pnl.costs());
            ImGui::SetItemTooltip("Slippage %s\nSpread     %s\nCommission %s\nSwap       %s",
                                  money(-p.pnl.slippage).c_str(), money(-p.pnl.spread).c_str(),
                                  money(-p.pnl.commission).c_str(), money(-p.pnl.swap).c_str());
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
    ImGui::TextDisabled("Buy %s  /  Sell %s lots   |   Gross %s   Costs -$%.2f   Net", lots(buyLots).c_str(),
                        lots(sellLots).c_str(), money(snap.live.gross).c_str(), snap.live.costs());
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
    if (ImGui::BeginTable("orders", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 84.0f);
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


void DashboardWindow::renderLotPreview(const LiveSettings& s) {
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
    if (snap) {
        ImGui::TextDisabled("This session (since %s): %zu basket(s), booked net %s",
                            snap->sessionStartedAt.empty() ? "--" : snap->sessionStartedAt.c_str(),
                            snap->baskets.size(), money(snap->realizedPnl).c_str());
    }
    ImGui::Separator();

    ImGui::BeginChild("history_list", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
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
    constexpr std::size_t kMaxShown = 500;
    const std::size_t shown = std::min(history_.size(), kMaxShown);
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
        ImGui::TextDisabled("%zu position(s)  |  %s  |  %s  |  closed %s", b.legs.size(), b.closeReason.c_str(),
                            duration(b.durationSeconds).c_str(), b.closedAt.c_str());
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
        ImGui::TextDisabled("... %zu older basket(s) - see the PDF report or the history file.", history_.size() - shown);
    }
    ImGui::EndChild();
}

void DashboardWindow::requestExit(LiveController& controller) {
    if (exitApproved_) return;
    const std::optional<LiveSnapshot> snap = controller.snapshot();
    const bool exposure = snap && (!snap->positions.empty() || !snap->pendingOrders.empty());
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
        "Commission is your configured rate x lots. Baskets count on the day they closed.",
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
    return a;
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

}  // namespace muuk::desktop
