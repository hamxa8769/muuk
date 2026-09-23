#include <algorithm>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
// Prevents windows.h from pulling in the legacy winsock.h, which conflicts
// with Asio's winsock2.h (included transitively via live_controller.hpp ->
// muuk/ctrader_fix headers) — must be defined before windows.h itself.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // keep windows.h's min/max macros from breaking std::min/std::max
#include <windows.h>
#endif

#include <GL/gl.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "dashboard_window.hpp"
#include "live_controller.hpp"

namespace {
void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

ImFont* tryAddFont(const char* path, float size, const ImWchar* ranges) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return nullptr;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, nullptr, ranges);
}
}  // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1360, 860, "Muuk \xe2\x80\x94 XAUUSD Range Bot", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 1100, 700, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // fixed single-window layout -- nothing worth persisting

    // Segoe UI (every Windows install has it) reads far better than ImGui's
    // built-in pixel font; fall back to the built-in one elsewhere. The
    // range covers Latin-1 plus general punctuation (dashes etc. used in
    // log lines).
    static const ImWchar kGlyphRanges[] = {0x0020, 0x00FF, 0x2000, 0x206F, 0};
    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    const float base = 16.0f * std::max(1.0f, xscale);
    ImFont* regular = tryAddFont("C:/Windows/Fonts/segoeui.ttf", base, kGlyphRanges);
    if (!regular) io.Fonts->AddFontDefault();
    ImFont* bold = tryAddFont("C:/Windows/Fonts/segoeuib.ttf", base, kGlyphRanges);
    ImFont* large = tryAddFont("C:/Windows/Fonts/segoeuib.ttf", base * 1.4f, kGlyphRanges);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    muuk::desktop::LiveController controller;
    muuk::desktop::DashboardWindow dashboard;
    dashboard.setFonts(bold, large);

    while (!glfwWindowShouldClose(window)) {
        // Wakes immediately on any input, and otherwise at least ~60x/s so
        // live prices/P&L keep moving -- smooth without spinning a core.
        glfwWaitEventsTimeout(1.0 / 60.0);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        dashboard.render(controller);

        ImGui::Render();
        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    controller.stop();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
