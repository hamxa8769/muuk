#include "dashboard_window.hpp"

#include <imgui.h>

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

}  // namespace

void DashboardWindow::render(LiveController& controller) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Muuk — Live Control", nullptr,
                  ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoTitleBar);

    const bool running = controller.isRunning();

    ImGui::TextUnformatted("Credentials file:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(400);
    ImGui::InputText("##configpath", configPathBuffer_.data(), configPathBuffer_.size());

    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Start")) {
        controller.start(std::string(configPathBuffer_.data()));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop")) {
        controller.stop();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextColored(running ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                        running ? "Connected" : "Disconnected");

    const std::string err = controller.lastError();
    if (!err.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", err.c_str());
    }

    ImGui::Separator();

    if (const auto snap = controller.snapshot()) {
        if (ImGui::BeginTable("status", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Symbol");
            ImGui::TableNextColumn();
            ImGui::Text("%s (id %s)", snap->symbolName.c_str(), snap->symbolId.c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Strategy");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(statusToString(snap->status));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Range");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f - %.2f", snap->rangeLower, snap->rangeUpper);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Active orders");
            ImGui::TableNextColumn();
            ImGui::Text("%d", snap->activeOrderCount);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Bid / Ask");
            ImGui::TableNextColumn();
            if (snap->bid && snap->ask) {
                ImGui::Text("%.2f / %.2f  (spread %.2f)", *snap->bid, *snap->ask, *snap->ask - *snap->bid);
            } else {
                ImGui::TextUnformatted("(no quote yet)");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Basket Gross P/L");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", snap->lastGrossPnlMoney);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Basket Net P/L");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", snap->lastNetPnlMoney);

            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("Not connected yet.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Log");
    ImGui::BeginChild("log", ImVec2(0, 0), true);
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
