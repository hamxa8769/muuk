#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
// Prevents windows.h from pulling in the legacy winsock.h, which conflicts
// with Asio's winsock2.h (included transitively via live_controller.hpp ->
// muuk/ctrader_fix headers) — must be defined before windows.h itself.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // keep windows.h's min/max macros from breaking std::min/std::max
#include <windows.h>
#include <dwmapi.h>
#endif

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

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
    ImFontConfig cfg;
    cfg.OversampleH = 3;  // smoother glyph edges at small sizes
    cfg.OversampleV = 2;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg, ranges);
}

// Matches the native Windows title bar to the app theme (Windows 10 20H1+;
// older builds used attribute 19 for the same thing). Harmless no-op elsewhere.
void setNativeTitleBarDark(GLFWwindow* window, bool dark) {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    const BOOL value = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value)))) {
        DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
#else
    (void)window;
    (void)dark;
#endif
}
#ifdef _WIN32
// Exit guard for Windows logoff/shutdown/restart (e.g. a VPS applying
// updates): the window gets WM_QUERYENDSESSION instead of a normal close,
// and there's no time for a dialog. With trades open, close them all right
// here -- showing "Muuk is closing open positions" on the shutdown screen
// while it waits -- before letting Windows continue.
muuk::desktop::LiveController* g_controller = nullptr;
WNDPROC g_prevWndProc = nullptr;

LRESULT CALLBACK guardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_QUERYENDSESSION && g_controller && g_controller->isRunning()) {
        const auto snap = g_controller->snapshot();
        if (snap && (!snap->positions.empty() || !snap->pendingOrders.empty())) {
            ShutdownBlockReasonCreate(hwnd, L"Muuk is closing open positions and pending orders");
            (void)g_controller->flattenAndWait(std::chrono::seconds(8));
            ShutdownBlockReasonDestroy(hwnd);
        }
    }
    if (msg == WM_ENDSESSION && wParam && g_controller) {
        g_controller->stop();  // the process ends right after this -- disconnect cleanly and save the log
    }
    return CallWindowProcW(g_prevWndProc, hwnd, msg, wParam, lParam);
}
#endif
}  // namespace

int main(int argc, char** argv) {
    // Optional: --config <file> picks the config; --connect connects on
    // launch (for an unattended VPS that restarts the app on boot).
    std::string configArg;
    bool connectOnLaunch = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configArg = argv[++i];
        } else if (arg == "--connect") {
            connectOnLaunch = true;
        }
    }

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
    ImFont* bold = tryAddFont("C:/Windows/Fonts/seguisb.ttf", base, kGlyphRanges);  // Segoe UI Semibold
    if (!bold) bold = tryAddFont("C:/Windows/Fonts/segoeuib.ttf", base, kGlyphRanges);
    ImFont* large = tryAddFont("C:/Windows/Fonts/segoeuib.ttf", base * 1.45f, kGlyphRanges);
    // Bahnschrift (Windows 10+) for the big KPI numbers -- a clean, modern
    // figure style; Segoe UI Semibold if it's missing.
    ImFont* numbers = tryAddFont("C:/Windows/Fonts/bahnschrift.ttf", base * 1.55f, kGlyphRanges);
    if (!numbers) numbers = tryAddFont("C:/Windows/Fonts/seguisb.ttf", base * 1.45f, kGlyphRanges);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    muuk::desktop::LiveController controller;
    muuk::desktop::DashboardWindow dashboard;
    dashboard.setFonts(bold, large, numbers);
    dashboard.setThemeChangedCallback([window](bool dark) { setNativeTitleBarDark(window, dark); });
    if (!configArg.empty()) dashboard.setConfigPath(configArg);
    // --connect, or the "Connect when the app opens" preference.
    if ((connectOnLaunch || dashboard.autoConnect()) && !dashboard.configPath().empty()) {
        controller.start(dashboard.configPath());
    }

#ifdef _WIN32
    // Installed after ImGui's backend (which subclasses the window too) and
    // removed before it shuts down, so the chain unwinds in order.
    g_controller = &controller;
    HWND hwnd = glfwGetWin32Window(window);
    g_prevWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&guardWndProc)));
#endif

    while (!dashboard.exitApproved()) {
        // Wakes immediately on any input, and otherwise at least ~60x/s so
        // live prices/P&L keep moving -- smooth without spinning a core.
        glfwWaitEventsTimeout(1.0 / 60.0);

        // Exit guard: the close button (or Alt+F4) only *asks* to exit; with
        // trades still open the dashboard shows a dialog first.
        if (glfwWindowShouldClose(window)) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            dashboard.requestExit(controller);
            if (dashboard.exitApproved()) break;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        dashboard.render(controller);

        ImGui::Render();
        int displayW = 0;
        int displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // fully covered by the glass backdrop every frame
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    controller.stop();

#ifdef _WIN32
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWndProc));
    g_controller = nullptr;
#endif

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
