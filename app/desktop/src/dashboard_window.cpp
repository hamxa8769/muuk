#include "dashboard_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "config_discovery.hpp"

namespace muuk::desktop {

using muuk::core::EngineStatus;
using muuk::core::OrderSide;
using muuk::ctrader_fix::LiveSettings;
using muuk::ctrader_fix::LiveSnapshot;

namespace {

// --- Palette ------------------------------------------------------------
const ImVec4 kGreen(0.19f, 0.75f, 0.45f, 1.0f);
const ImVec4 kRed(0.90f, 0.33f, 0.30f, 1.0f);
const ImVec4 kBlue(0.30f, 0.55f, 0.96f, 1.0f);
const ImVec4 kAmber(0.88f, 0.64f, 0.23f, 1.0f);
const ImVec4 kGrey(0.55f, 0.58f, 0.64f, 1.0f);

ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

ImVec4 scaled(ImVec4 c, float f) {
    return ImVec4(std::min(c.x * f, 1.0f), std::min(c.y * f, 1.0f), std::min(c.z * f, 1.0f), c.w);
}

ImVec4 pnlColor(double v) {
    if (v > 0.005) return kGreen;
    if (v < -0.005) return kRed;
    return kGrey;
}

std::string money(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s$%.2f", v < 0 ? "-" : (v > 0 ? "+" : ""), std::fabs(v));
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

const char* sideName(OrderSide side) { return side == OrderSide::BuyStop ? "BUY" : "SELL"; }
const char* orderTypeName(OrderSide side) { return side == OrderSide::BuyStop ? "Buy Stop" : "Sell Stop"; }

// A filled button in `color`, the same size/shape as any other button.
bool coloredButton(const char* label, ImVec4 color, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, scaled(color, 1.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, scaled(color, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// A small rounded "chip" with text, for status labels.
void chip(const char* text, ImVec4 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pad(8.0f, 3.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + textSize.x + pad.x * 2, pos.y + textSize.y + pad.y * 2);
    dl->AddRectFilled(pos, end, ImGui::ColorConvertFloat4ToU32(withAlpha(color, 0.18f)), 10.0f);
    dl->AddRect(pos, end, ImGui::ColorConvertFloat4ToU32(withAlpha(color, 0.65f)), 10.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), ImGui::ColorConvertFloat4ToU32(color), text);
    ImGui::Dummy(ImVec2(end.x - pos.x, end.y - pos.y));
}

void sectionTitle(const char* text, ImFont* bold) {
    if (bold) ImGui::PushFont(bold);
    ImGui::TextUnformatted(text);
    if (bold) ImGui::PopFont();
}

struct CycleState {
    const char* label;
    ImVec4 color;
};

CycleState cycleState(const std::optional<LiveSnapshot>& snap, bool connected) {
    if (!connected || !snap) return {"Disconnected", kGrey};
    if (snap->closeFailed) return {"CLOSE FAILED - needs attention", kRed};
    if (snap->closingInProgress) return {"Closing basket...", kAmber};
    if (snap->cooldownRemainingSeconds > 0) return {"Cooldown before next range", kBlue};
    switch (snap->status) {
        case EngineStatus::Active:
            return snap->seeding ? CycleState{"Seeding - waiting for first fill", kAmber}
                                 : CycleState{"Range active", kGreen};
        case EngineStatus::HaltedByRisk:
            return {"Halted by risk limit", kRed};
        case EngineStatus::Completed:
            return snap->settings.autoContinue ? CycleState{"Basket booked - next range soon", kBlue}
                                               : CycleState{"Basket booked - press Open range", kBlue};
        case EngineStatus::Idle:
        default:
            return {"Idle - press Open range", kGrey};
    }
}

// Why "Open range" is unavailable right now, or nullptr if it's available.
const char* openRangeBlocker(const std::optional<LiveSnapshot>& snap, bool connected) {
    if (!connected || !snap) return "Connect first.";
    if (!snap->bid || !snap->ask) return "Waiting for the first live quote.";
    if (snap->closeFailed) return "A closing trade was rejected - close leftovers manually, then reconnect.";
    if (snap->status == EngineStatus::Active) return "A range is already running - close it first.";
    if (snap->closingInProgress) return "Previous basket is still closing.";
    if (!snap->positions.empty()) return "Previous range still has open positions.";
    if (!snap->pendingOrders.empty()) return "Previous range still has pending orders.";
    return nullptr;
}

// Draws the range as a horizontal price scale: the band between the two
// boundaries, pending orders as triangles, open positions as dots, and the
// live bid/ask as a vertical marker.
void drawRangeChart(const LiveSnapshot& snap, float height) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = avail.x;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 trackCol = ImGui::GetColorU32(ImGuiCol_FrameBg);

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

    // Vertical layout (top to bottom): live price label, Buy Stop markers
    // (pointing up), the track with range band / positions, Sell Stop
    // markers (pointing down), boundary price labels.
    const float textH = ImGui::GetTextLineHeight();
    const float left = origin.x + 6.0f;
    const float right = origin.x + width - 6.0f;
    const float trackY = origin.y + textH + 34.0f;
    auto xOf = [&](double p) { return left + static_cast<float>((p - lo) / (hi - lo)) * (right - left); };

    dl->AddRectFilled(ImVec2(left, trackY - 3), ImVec2(right, trackY + 3), trackCol, 3.0f);

    if (snap.rangeUpper > snap.rangeLower) {
        const float x0 = xOf(snap.rangeLower);
        const float x1 = xOf(snap.rangeUpper);
        dl->AddRectFilled(ImVec2(x0, trackY - 12), ImVec2(x1, trackY + 12),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(kBlue, 0.16f)), 4.0f);
        dl->AddLine(ImVec2(x0, trackY - 15), ImVec2(x0, trackY + 15), ImGui::ColorConvertFloat4ToU32(kRed), 2.0f);
        dl->AddLine(ImVec2(x1, trackY - 15), ImVec2(x1, trackY + 15), ImGui::ColorConvertFloat4ToU32(kGreen), 2.0f);
        const std::string loText = price(snap.rangeLower);
        const std::string hiText = price(snap.rangeUpper);
        const float labelY = trackY + 30.0f;
        dl->AddText(ImVec2(x0 - ImGui::CalcTextSize(loText.c_str()).x / 2, labelY), dimCol, loText.c_str());
        dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(hiText.c_str()).x / 2, labelY), dimCol, hiText.c_str());
    }

    for (const auto& o : snap.pendingOrders) {
        const float x = xOf(o.price);
        if (o.side == OrderSide::BuyStop) {
            dl->AddTriangleFilled(ImVec2(x, trackY - 28), ImVec2(x - 6, trackY - 18), ImVec2(x + 6, trackY - 18),
                                  ImGui::ColorConvertFloat4ToU32(kGreen));
        } else {
            dl->AddTriangleFilled(ImVec2(x, trackY + 28), ImVec2(x + 6, trackY + 18), ImVec2(x - 6, trackY + 18),
                                  ImGui::ColorConvertFloat4ToU32(kRed));
        }
    }
    for (const auto& p : snap.positions) {
        const float x = xOf(p.entryPrice);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(p.side == OrderSide::BuyStop ? kGreen : kRed);
        dl->AddCircleFilled(ImVec2(x, trackY), 5.0f, col);
        dl->AddCircle(ImVec2(x, trackY), 5.0f, textCol, 0, 1.0f);
    }

    if (snap.bid && snap.ask) {
        const double mid = (*snap.bid + *snap.ask) / 2.0;
        const float x = xOf(mid);
        dl->AddLine(ImVec2(x, origin.y + textH + 2), ImVec2(x, trackY + 12), textCol, 1.5f);
        const std::string midText = price(mid);
        const float tw = ImGui::CalcTextSize(midText.c_str()).x;
        const float tx = std::clamp(x - tw / 2, left, right - tw);
        dl->AddText(ImVec2(tx, origin.y), textCol, midText.c_str());
    }
}

// One KPI card: a small caption, a big value, and an optional sub-line.
void kpiCard(const char* id, const char* caption, const std::string& value, ImVec4 valueColor, const std::string& sub,
             ImFont* large, float width, float height) {
    ImGui::BeginChild(id, ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::TextDisabled("%s", caption);
    if (large) ImGui::PushFont(large);
    ImGui::TextColored(valueColor, "%s", value.c_str());
    if (large) ImGui::PopFont();
    if (!sub.empty()) ImGui::TextDisabled("%s", sub.c_str());
    ImGui::EndChild();
}

// Numeric settings field; marks `dirty` when edited.
bool settingDouble(const char* label, const char* help, double* v, double step, const char* fmt, bool* dirty) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    const bool changed = ImGui::InputDouble("##v", v, step, step * 10.0, fmt);
    ImGui::PopID();
    if (changed) *dirty = true;
    return changed;
}

bool settingInt(const char* label, const char* help, int* v, bool* dirty) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help) ImGui::SetItemTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    const bool changed = ImGui::InputInt("##v", v);
    ImGui::PopID();
    if (changed) *dirty = true;
    return changed;
}

void clampSettings(LiveSettings& s) {
    s.rangeWidth = std::max(0.05, s.rangeWidth);
    s.seedOffset = std::max(0.0, s.seedOffset);
    s.initialLot = std::max(0.01, s.initialLot);
    s.lotIncrement = std::max(0.0, s.lotIncrement);
    s.profitTarget = std::max(0.0, s.profitTarget);
    s.cooldownSeconds = std::max(0, s.cooldownSeconds);
    s.maxOrders = std::max(1, s.maxOrders);
    s.maxLot = std::max(0.01, s.maxLot);
    s.maxBasketLoss = std::max(0.0, s.maxBasketLoss);
}

}  // namespace

DashboardWindow::DashboardWindow() {
    if (const auto found = findDefaultConfigPath()) {
        const std::string& path = *found;
        const std::size_t n = std::min(path.size(), configPathBuffer_.size() - 1);
        std::copy_n(path.begin(), n, configPathBuffer_.begin());
        configPathBuffer_[n] = '\0';
    }
    applyTheme();
}

void DashboardWindow::setFonts(ImFont* bold, ImFont* large) {
    boldFont_ = bold;
    largeFont_ = large;
}

void DashboardWindow::flash(const std::string& message) {
    flashMessage_ = message;
    flashUntil_ = ImGui::GetTime() + 4.0;
}

void DashboardWindow::applyTheme() const {
    ImGuiStyle& style = ImGui::GetStyle();
    if (darkMode_) {
        ImGui::StyleColorsDark();
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.075f, 0.090f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.090f, 0.102f, 0.125f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.100f, 0.112f, 0.137f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.165f, 0.185f, 0.227f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.122f, 0.137f, 0.169f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.153f, 0.173f, 0.212f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.180f, 0.204f, 0.251f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.153f, 0.180f, 0.235f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.196f, 0.235f, 0.310f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.227f, 0.278f, 0.369f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.141f, 0.188f, 0.267f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.180f, 0.235f, 0.333f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.212f, 0.282f, 0.400f, 1.0f);
        c[ImGuiCol_Tab] = ImVec4(0.110f, 0.125f, 0.157f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.196f, 0.255f, 0.361f, 1.0f);
        c[ImGuiCol_TabSelected] = ImVec4(0.161f, 0.212f, 0.302f, 1.0f);
        c[ImGuiCol_TableHeaderBg] = ImVec4(0.110f, 0.125f, 0.157f, 1.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.025f);
        c[ImGuiCol_Text] = ImVec4(0.905f, 0.915f, 0.935f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.540f, 0.575f, 0.640f, 1.0f);
        c[ImGuiCol_CheckMark] = kBlue;
        c[ImGuiCol_PlotHistogram] = kGreen;
    } else {
        ImGui::StyleColorsLight();
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.955f, 0.960f, 0.970f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        c[ImGuiCol_Border] = ImVec4(0.840f, 0.855f, 0.880f, 1.0f);
        c[ImGuiCol_FrameBg] = ImVec4(0.925f, 0.933f, 0.949f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.878f, 0.894f, 0.925f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.820f, 0.847f, 0.898f, 1.0f);
        c[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.0f, 0.0f, 0.025f);
        c[ImGuiCol_Text] = ImVec4(0.110f, 0.125f, 0.157f, 1.0f);
        c[ImGuiCol_CheckMark] = kBlue;
        c[ImGuiCol_PlotHistogram] = kGreen;
    }

    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.CellPadding = ImVec2(8, 5);
    style.ScrollbarSize = 12.0f;
}

void DashboardWindow::render(LiveController& controller) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Muuk", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    const bool running = controller.isRunning();
    const std::optional<LiveSnapshot> snap = controller.snapshot();
    const bool connected = running && snap && snap->connected;

    // Load the tunable settings from the config file once, so they can be
    // reviewed/edited before connecting; while connected (and not being
    // edited), mirror whatever the running bot actually uses.
    if (!settingsLoaded_ && configPathBuffer_[0] != '\0') {
        if (auto loaded = controller.loadSettings(configPathBuffer_.data())) {
            editSettings_ = *loaded;
        }
        settingsLoaded_ = true;
    }
    if (connected && !settingsDirty_) {
        editSettings_ = snap->settings;
        settingsLoaded_ = true;
    }

    renderConnectionBar(controller, running, connected);
    ImGui::Spacing();
    renderKpiCards(snap, connected);
    ImGui::Spacing();

    const float leftWidth = std::max(360.0f, ImGui::GetContentRegionAvail().x * 0.34f);
    ImGui::BeginChild("left", ImVec2(leftWidth, 0), ImGuiChildFlags_None);
    renderRangePanel(controller, snap, connected);
    ImGui::Spacing();
    renderSettingsPanel(controller, snap, connected);
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("tabs")) {
        const std::string posLabel =
            "Positions (" + std::to_string(snap ? snap->positions.size() : 0) + ")###positions";
        if (ImGui::BeginTabItem(posLabel.c_str())) {
            if (snap) {
                renderPositionsTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            ImGui::EndTabItem();
        }
        const std::string ordLabel =
            "Pending orders (" + std::to_string(snap ? snap->pendingOrders.size() : 0) + ")###orders";
        if (ImGui::BeginTabItem(ordLabel.c_str())) {
            if (snap) {
                renderOrdersTab(controller, *snap, connected);
            } else {
                ImGui::TextDisabled("Not connected.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            renderLogTab(controller);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    if (openConfirmRequested_) {
        ImGui::OpenPopup("Please confirm");
        openConfirmRequested_ = false;
    }
    renderConfirmPopup(controller);

    ImGui::End();
}

void DashboardWindow::renderConnectionBar(LiveController& controller, bool running, bool connected) {
    if (largeFont_) ImGui::PushFont(largeFont_);
    ImGui::TextColored(kBlue, "MUUK");
    if (largeFont_) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("XAUUSD range bot");

    ImGui::SameLine(0.0f, 24.0f);
    if (connected) {
        chip("Connected", kGreen);
    } else if (running) {
        chip("Connecting...", kAmber);
    } else {
        chip("Disconnected", kGrey);
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Config");
    ImGui::SameLine();
    const float themeWidth = 90.0f;
    const float buttonsWidth = 230.0f;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - buttonsWidth - themeWidth));
    ImGui::BeginDisabled(running);
    if (ImGui::InputText("##configpath", configPathBuffer_.data(), configPathBuffer_.size())) {
        settingsLoaded_ = false;  // reload tunables from the newly typed file
        settingsDirty_ = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (coloredButton("Connect", kGreen, ImVec2(105, 0))) {
        settingsDirty_ = false;
        controller.start(std::string(configPathBuffer_.data()));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Disconnect", ImVec2(105, 0))) {
        controller.stop();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Disconnects from the broker. Open positions and pending orders stay at the broker as they are.");

    ImGui::SameLine();
    bool dark = darkMode_;
    if (ImGui::Checkbox(dark ? "Dark" : "Light", &dark)) {
        darkMode_ = dark;
        applyTheme();
    }

    const std::string err = controller.lastError();
    if (!err.empty()) {
        ImGui::TextColored(kRed, "Error: %s", err.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) controller.clearError();
    }
    if (!flashMessage_.empty() && ImGui::GetTime() < flashUntil_) {
        ImGui::TextColored(kAmber, "%s", flashMessage_.c_str());
    }
}

void DashboardWindow::renderKpiCards(const std::optional<LiveSnapshot>& snap, bool connected) {
    const int count = 6;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * (count - 1)) / count;
    const float height = ImGui::GetTextLineHeightWithSpacing() * 3.2f + (largeFont_ ? 8.0f : 0.0f);

    const bool haveQuote = snap && snap->bid && snap->ask;

    kpiCard("k_price", "Bid / Ask",
            haveQuote ? price(*snap->bid) + " / " + price(*snap->ask) : std::string("--"),
            ImGui::GetStyleColorVec4(ImGuiCol_Text), snap ? snap->symbolName + "  (id " + snap->symbolId + ")" : "",
            largeFont_, width, height);
    ImGui::SameLine();

    kpiCard("k_spread", "Spread", haveQuote ? price(*snap->ask - *snap->bid) : std::string("--"),
            ImGui::GetStyleColorVec4(ImGuiCol_Text),
            snap ? "Range " + price(snap->rangeLower) + " - " + price(snap->rangeUpper) : "", largeFont_, width,
            height);
    ImGui::SameLine();

    double floating = 0.0;
    double buyLots = 0.0;
    double sellLots = 0.0;
    if (snap) {
        for (const auto& p : snap->positions) {
            floating += p.floatingPnl;
            (p.side == OrderSide::BuyStop ? buyLots : sellLots) += p.lots;
        }
    }
    kpiCard("k_float", "Floating P/L", connected ? money(floating) : std::string("--"), pnlColor(floating),
            snap ? "Target " + money(snap->settings.profitTarget) : "", largeFont_, width, height);
    ImGui::SameLine();

    kpiCard("k_pos", "Open positions", snap ? std::to_string(snap->positions.size()) : std::string("--"),
            ImGui::GetStyleColorVec4(ImGuiCol_Text),
            snap ? "Buy " + lots(buyLots) + "  |  Sell " + lots(sellLots) : "", largeFont_, width, height);
    ImGui::SameLine();

    kpiCard("k_ord", "Pending orders", snap ? std::to_string(snap->pendingOrders.size()) : std::string("--"),
            ImGui::GetStyleColorVec4(ImGuiCol_Text), cycleState(snap, connected).label, largeFont_, width, height);
    ImGui::SameLine();

    kpiCard("k_real", "Session booked P/L", snap ? money(snap->realizedPnl) : std::string("--"),
            snap ? pnlColor(snap->realizedPnl) : kGrey,
            snap ? std::to_string(snap->basketsClosed) + " basket(s) closed" : "", largeFont_, width, height);
}

void DashboardWindow::renderRangePanel(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                       bool connected) {
    ImGui::BeginChild("range_panel", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    sectionTitle("Range", boldFont_);
    ImGui::SameLine();
    const CycleState state = cycleState(snap, connected);
    chip(state.label, state.color);
    if (snap && snap->cooldownRemainingSeconds > 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kBlue, "%ds", snap->cooldownRemainingSeconds);
    }

    const float chartHeight = ImGui::GetTextLineHeight() * 2.0f + 70.0f;
    if (snap && connected) {
        drawRangeChart(*snap, chartHeight);
        ImGui::TextDisabled("Width $%.2f   |   green = Buy Stop / buy, red = Sell Stop / sell",
                            snap->rangeUpper - snap->rangeLower);
    } else {
        ImGui::Dummy(ImVec2(0, chartHeight));
        ImGui::TextDisabled("Connect to see the live range.");
    }

    // Progress towards the profit target.
    if (snap && connected && snap->settings.profitTarget > 0.0) {
        double floating = 0.0;
        for (const auto& p : snap->positions) floating += p.floatingPnl;
        const float frac = static_cast<float>(std::clamp(floating / snap->settings.profitTarget, 0.0, 1.0));
        const std::string overlay = money(floating) + " of " + money(snap->settings.profitTarget) + " target";
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, floating >= 0 ? kGreen : kRed);
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), overlay.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    const float bigHeight = ImGui::GetFrameHeight() * 1.6f;

    const char* blocker = openRangeBlocker(snap, connected);
    ImGui::BeginDisabled(blocker != nullptr);
    if (coloredButton("Open range", kGreen, ImVec2(half, bigHeight))) {
        if (!controller.openRange()) flash("Could not open a range - see the Log tab for why.");
    }
    ImGui::EndDisabled();
    if (blocker) ImGui::SetItemTooltip("%s", blocker);
    else ImGui::SetItemTooltip("Opens a new range centered on the current price.");

    ImGui::SameLine();
    const bool somethingToClose =
        connected && snap &&
        (snap->status == EngineStatus::Active || !snap->positions.empty() || !snap->pendingOrders.empty());
    ImGui::BeginDisabled(!somethingToClose);
    if (coloredButton("Close range", kRed, ImVec2(half, bigHeight))) {
        confirm_ = Confirm::CloseRange;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Cancels all pending orders, closes all open positions, and stops the bot until you open a new range.");

    ImGui::BeginDisabled(!connected || !snap);
    bool autoContinue = snap ? snap->settings.autoContinue : editSettings_.autoContinue;
    if (ImGui::Checkbox("Auto-open the next range after each basket closes", &autoContinue) && snap) {
        LiveSettings s = snap->settings;  // only this one field -- other edits wait for Apply
        s.autoContinue = autoContinue;
        controller.applySettings(s);
        editSettings_.autoContinue = autoContinue;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("Emergency");
    ImGui::BeginDisabled(!connected || !snap || snap->positions.empty());
    if (ImGui::Button("Close all positions", ImVec2(half, 0))) {
        confirm_ = Confirm::CloseAllPositions;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected || !snap || snap->pendingOrders.empty());
    if (ImGui::Button("Cancel all pending", ImVec2(half, 0))) {
        confirm_ = Confirm::CancelAllPending;
        openConfirmRequested_ = true;
    }
    ImGui::EndDisabled();

    ImGui::EndChild();
}

void DashboardWindow::renderSettingsPanel(LiveController& controller, const std::optional<LiveSnapshot>& snap,
                                          bool connected) {
    ImGui::BeginChild("settings_panel", ImVec2(0, 0), ImGuiChildFlags_Borders);
    sectionTitle("Strategy settings", boldFont_);
    if (settingsDirty_) {
        ImGui::SameLine();
        chip("edited", kAmber);
    }
    ImGui::TextDisabled("Changes apply from the next range - never to a running one.");

    ImGui::BeginDisabled(!settingsLoaded_);
    if (ImGui::BeginTable("settings", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        LiveSettings& s = editSettings_;
        settingDouble("Range width ($)", "Distance between the Buy Stop (top) and Sell Stop (bottom) boundaries.",
                      &s.rangeWidth, 0.10, "%.2f", &settingsDirty_);
        settingDouble("Profit target ($)", "Floating net profit at which the whole basket is booked and closed.",
                      &s.profitTarget, 0.25, "%.2f", &settingsDirty_);
        settingDouble("First lot", "Lot size of the first order in each range.", &s.initialLot, 0.01, "%.2f",
                      &settingsDirty_);
        settingDouble("Lot increment", "Added to the lot size on every following order (0.01 -> 0.02 -> 0.03...).",
                      &s.lotIncrement, 0.01, "%.2f", &settingsDirty_);
        settingDouble("Seed offset ($)",
                      "How far beyond bid/ask the two starting orders sit. The first to fill anchors the range; "
                      "0 = place the full range immediately instead.",
                      &s.seedOffset, 0.01, "%.2f", &settingsDirty_);
        settingInt("Cooldown (s)", "Pause after a basket is fully closed before the next range opens.",
                   &s.cooldownSeconds, &settingsDirty_);
        settingInt("Max orders", "Safety limit: no more orders are placed in a range beyond this many.", &s.maxOrders,
                   &settingsDirty_);
        settingDouble("Max lot", "Safety limit on any single order's lot size.", &s.maxLot, 0.10, "%.2f",
                      &settingsDirty_);
        settingDouble("Max basket loss ($)", "Safety limit: stop placing new orders past this floating loss. 0 = off.",
                      &s.maxBasketLoss, 10.0, "%.2f", &settingsDirty_);
        clampSettings(s);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    const float third = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    ImGui::BeginDisabled(!connected || !settingsDirty_);
    if (coloredButton("Apply", kBlue, ImVec2(third, 0))) {
        controller.applySettings(editSettings_);
        settingsDirty_ = false;
        flash("Settings applied - they take effect when the next range opens.");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Use these settings for the running bot, from its next range on.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!settingsLoaded_ || configPathBuffer_[0] == '\0');
    if (ImGui::Button("Save to file", ImVec2(third, 0))) {
        if (controller.saveSettings(configPathBuffer_.data(), editSettings_)) {
            if (connected) controller.applySettings(editSettings_);
            settingsDirty_ = false;
            flash("Settings saved to the config file.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Writes these settings into the config file (credentials are left untouched) and applies them if connected.");

    ImGui::SameLine();
    ImGui::BeginDisabled(!settingsDirty_);
    if (ImGui::Button("Revert", ImVec2(third, 0))) {
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
    if (snap.positions.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No open positions.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (ImGui::BeginTable("positions", 7, flags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Side", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Lots");
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("Now");
        ImGui::TableSetupColumn("P/L");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (const auto& p : snap.positions) {
            ImGui::PushID(static_cast<int>(p.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(p.id));
            ImGui::TableNextColumn();
            ImGui::TextColored(p.side == OrderSide::BuyStop ? kGreen : kRed, "%s", sideName(p.side));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lots(p.lots).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(price(p.entryPrice).c_str());
            ImGui::TableNextColumn();
            if (snap.bid && snap.ask) {
                ImGui::TextUnformatted(price(p.side == OrderSide::BuyStop ? *snap.bid : *snap.ask).c_str());
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(pnlColor(p.floatingPnl), "%s", money(p.floatingPnl).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            if (coloredButton("Close", kRed, ImVec2(-FLT_MIN, 0))) controller.closePosition(p.id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    double total = 0.0;
    double buyLots = 0.0;
    double sellLots = 0.0;
    for (const auto& p : snap.positions) {
        total += p.floatingPnl;
        (p.side == OrderSide::BuyStop ? buyLots : sellLots) += p.lots;
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Buy %s lots   |   Sell %s lots   |   Total floating", lots(buyLots).c_str(), lots(sellLots).c_str());
    ImGui::SameLine();
    ImGui::TextColored(pnlColor(total), "%s", money(total).c_str());
}

void DashboardWindow::renderOrdersTab(LiveController& controller, const LiveSnapshot& snap, bool connected) {
    if (snap.pendingOrders.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No pending orders.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("orders", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Lots");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Distance");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (const auto& o : snap.pendingOrders) {
            ImGui::PushID(static_cast<int>(o.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%llu", static_cast<unsigned long long>(o.id));
            ImGui::TableNextColumn();
            ImGui::TextColored(o.side == OrderSide::BuyStop ? kGreen : kRed, "%s", orderTypeName(o.side));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(lots(o.lots).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(price(o.price).c_str());
            ImGui::TableNextColumn();
            if (snap.bid && snap.ask) {
                // How far price still has to travel to trigger it.
                const double trigger = o.side == OrderSide::BuyStop ? *snap.ask : *snap.bid;
                ImGui::TextDisabled("$%.2f away", std::fabs(o.price - trigger));
            } else {
                ImGui::TextDisabled("--");
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) controller.cancelOrder(o.id);
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
    if (ImGui::Button("Copy")) {
        std::string all;
        for (const auto& l : lines) {
            all += l;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
        flash("Log copied to clipboard.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) controller.clearLog();

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
            ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (l.find("CRITICAL") != std::string::npos || l.find("[error]") != std::string::npos ||
                l.find("rejected") != std::string::npos) {
                col = kRed;
            } else if (l.find("[manual]") != std::string::npos || l.find("[settings]") != std::string::npos) {
                col = kAmber;
            } else if (l.find("[basket]") != std::string::npos) {
                col = kBlue;
            } else if (l.find("[order]") != std::string::npos) {
                col = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            }
            ImGui::TextColored(col, "%s", l.c_str());
        }
    }
    if (logAutoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void DashboardWindow::renderConfirmPopup(LiveController& controller) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Please confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const char* title = "";
    const char* detail = "";
    const char* action = "";
    switch (confirm_) {
        case Confirm::CloseRange:
            title = "Close the running range?";
            detail = "All pending orders are cancelled and every open position is closed at market.\n"
                     "The bot then waits until you press Open range.";
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
        case Confirm::None:
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
    }

    sectionTitle(title, boldFont_);
    ImGui::TextDisabled("%s", detail);
    ImGui::Spacing();
    if (coloredButton(action, kRed, ImVec2(180, 0))) {
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
            case Confirm::None:
                break;
        }
        confirm_ = Confirm::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        confirm_ = Confirm::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace muuk::desktop
