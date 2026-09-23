#include "dashboard_window.hpp"

#include <algorithm>
#include <cstdarg>

#include <imgui.h>

#include "config_discovery.hpp"

namespace muuk::desktop {

namespace {

const char* statusToString(muuk::core::EngineStatus status) {
    switch (status) {
        case muuk::core::EngineStatus::Idle:
            return "Idle";
        case muuk::core::EngineStatus::Active:
            return "ACTIVE";
        case muuk::core::EngineStatus::HaltedByRisk:
            return "HALTED (risk limit)";
        case muuk::core::EngineStatus::Completed:
            return "Completed";
    }
    return "?";
}

ImVec4 statusColor(muuk::core::EngineStatus status, bool running) {
    if (!running) return ImVec4(0.60f, 0.60f, 0.63f, 1.0f);
    switch (status) {
        case muuk::core::EngineStatus::Active:
            return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
        case muuk::core::EngineStatus::HaltedByRisk:
            return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
        case muuk::core::EngineStatus::Completed:
            return ImVec4(0.40f, 0.65f, 1.00f, 1.0f);
        case muuk::core::EngineStatus::Idle:
        default:
            return ImVec4(0.85f, 0.70f, 0.25f, 1.0f);
    }
}

void statRow(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void statRow(const char* label, const char* fmt, ...) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

}  // namespace

DashboardWindow::DashboardWindow() {
    if (const auto found = findDefaultConfigPath()) {
        const std::string& path = *found;
        const std::size_t n = (path.size() < configPathBuffer_.size()) ? path.size() : configPathBuffer_.size() - 1;
        std::copy_n(path.begin(), n, configPathBuffer_.begin());
        configPathBuffer_[n] = '\0';
    }
    applyTheme();
}

void DashboardWindow::applyTheme() const {
    if (darkMode_) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(10, 8);
}

void DashboardWindow::render(LiveController& controller) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Muuk — Live Control", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoTitleBar);

    // --- Header: title + theme toggle -----------------------------------
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "MUUK");
    ImGui::SameLine();
    ImGui::TextDisabled("XAUUSD pending-order strategy — live control");
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    if (ImGui::Checkbox(darkMode_ ? "Dark" : "Light", &darkMode_)) {
        applyTheme();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Connection");

    const bool running = controller.isRunning();

    ImGui::TextUnformatted("Credentials file");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() - 380);
    ImGui::InputText("##configpath", configPathBuffer_.data(), configPathBuffer_.size());

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Start", ImVec2(80, 0))) {
        controller.start(std::string(configPathBuffer_.data()));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(80, 0))) {
        controller.stop();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    {
        const auto snap = controller.snapshot();
        const ImVec4 color = statusColor(snap ? snap->status : muuk::core::EngineStatus::Idle, running);
        ImGui::TextColored(color, "%s", running ? "● Connected" : "○ Disconnected");
    }

    ImGui::SameLine(0.0f, 24.0f);
    ImGui::BeginDisabled(!running);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.16f, 0.14f, 1.0f));
    if (ImGui::Button("Close all positions", ImVec2(160, 0))) {
        ImGui::OpenPopup("Confirm close all positions");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel all pending orders", ImVec2(190, 0))) {
        ImGui::OpenPopup("Confirm cancel all pending");
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();

    // Both are manual, broker-facing overrides that also stop the bot from
    // trading on its own afterward (see LiveRunner::closeAllPositions'/
    // cancelAllPendingOrders' comments) -- a confirmation guards against a
    // stray click doing something that disruptive.
    if (ImGui::BeginPopupModal("Confirm close all positions", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Close every open position at the broker with individual Market orders?");
        ImGui::TextDisabled("The bot will stop trading until you Stop and Start it again.");
        ImGui::Spacing();
        if (ImGui::Button("Close all positions", ImVec2(160, 0))) {
            controller.closeAllPositions();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Confirm cancel all pending", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Cancel every still-pending (unfilled) order at the broker?");
        ImGui::TextDisabled("The bot will stop trading until you Stop and Start it again.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel all pending orders", ImVec2(190, 0))) {
            controller.cancelAllPendingOrders();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Dismiss", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const std::string err = controller.lastError();
    if (!err.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "Error: %s", err.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Status");

    if (const auto snap = controller.snapshot()) {
        if (ImGui::BeginTable("status", 2, ImGuiTableFlags_SizingFixedFit)) {
            statRow("Symbol", "%s (id %s)", snap->symbolName.c_str(), snap->symbolId.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Strategy");
            ImGui::TableNextColumn();
            ImGui::TextColored(statusColor(snap->status, running), "%s", statusToString(snap->status));
            statRow("Range", "%.2f - %.2f", snap->rangeLower, snap->rangeUpper);
            statRow("Active orders", "%d", snap->activeOrderCount);
            if (snap->bid && snap->ask) {
                statRow("Bid / Ask", "%.2f / %.2f   (spread %.2f)", *snap->bid, *snap->ask, *snap->ask - *snap->bid);
            } else {
                statRow("Bid / Ask", "(no quote yet)");
            }
            statRow("Basket Gross P/L", "%.2f", snap->lastGrossPnlMoney);
            statRow("Basket Net P/L", "%.2f", snap->lastNetPnlMoney);
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Not connected yet.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Log");
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Border);
    for (const auto& line : controller.logLines()) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace muuk::desktop
