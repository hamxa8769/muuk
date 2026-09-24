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

#ifdef _WIN32
#include <d3d11.h>
#else
#include <GL/gl.h>
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef _WIN32
#include <imgui_impl_dx11.h>
#else
#include <imgui_impl_opengl3.h>
#endif

#include "dashboard_window.hpp"
#include "glass_ui.hpp"
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
// The GPU side: Direct3D 11 on Windows (hardware, else the WARP software
// rasterizer -- so the app also opens on a VPS without a graphics card),
// OpenGL 3 elsewhere.
class Renderer {
public:
    // Before the window is created.
    static void windowHints() {
#ifdef _WIN32
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    }

    bool init(GLFWwindow* window) {
        window_ = window;
#ifdef _WIN32
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = glfwGetWin32Window(window);
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got{};
        // MUUK_SOFTWARE_RENDERER=1 forces the no-graphics-card path (to try
        // out what a GPU-less VPS will do).
        char* forced = nullptr;
        std::size_t forcedLen = 0;
        const bool forceSoftware = _dupenv_s(&forced, &forcedLen, "MUUK_SOFTWARE_RENDERER") == 0 && forced != nullptr &&
                                   forced[0] == '1';
        std::free(forced);
        HRESULT hr = E_FAIL;
        if (!forceSoftware) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                               D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got, &context_);
        }
        name_ = "Direct3D 11 (graphics card)";
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION,
                                               &sd, &swapChain_, &device_, &got, &context_);
            name_ = "Direct3D 11 software (no GPU)";
            software_ = true;
        }
        if (FAILED(hr)) return false;
        createTarget();
        ImGui_ImplGlfw_InitForOther(window, true);
        return ImGui_ImplDX11_Init(device_, context_);
#else
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        name_ = "OpenGL 3";
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        return ImGui_ImplOpenGL3_Init("#version 130");
#endif
    }

    void newFrame() {
#ifdef _WIN32
        ImGui_ImplDX11_NewFrame();
#else
        ImGui_ImplOpenGL3_NewFrame();
#endif
        ImGui_ImplGlfw_NewFrame();
    }

    void present() {
        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w <= 0 || h <= 0) return;  // minimized
#ifdef _WIN32
        if (w != width_ || h != height_) {
            releaseTarget();
            swapChain_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0);
            createTarget();
            width_ = w;
            height_ = h;
        }
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // fully covered by the glass backdrop every frame
        context_->OMSetRenderTargets(1, &target_, nullptr);
        context_->ClearRenderTargetView(target_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swapChain_->Present(1, 0);  // vsync
#else
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
#endif
    }

    void shutdown() {
#ifdef _WIN32
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        releaseTarget();
        if (swapChain_) swapChain_->Release();
        if (context_) context_->Release();
        if (device_) device_->Release();
        swapChain_ = nullptr;
        context_ = nullptr;
        device_ = nullptr;
#else
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
#endif
    }

    [[nodiscard]] const std::string& name() const { return name_; }
    // Drawing on the CPU: keep the frame rate down.
    [[nodiscard]] bool software() const { return software_; }

private:
#ifdef _WIN32
    void createTarget() {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
            device_->CreateRenderTargetView(back, nullptr, &target_);
            back->Release();
        }
    }
    void releaseTarget() {
        if (target_) target_->Release();
        target_ = nullptr;
    }
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* target_ = nullptr;
    int width_ = 0;
    int height_ = 0;
#endif
    GLFWwindow* window_ = nullptr;
    std::string name_;
    bool software_ = false;
};

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

    Renderer::windowHints();
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1360, 860, "Muuk \xe2\x80\x94 XAUUSD Range Bot", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 1100, 700, GLFW_DONT_CARE, GLFW_DONT_CARE);

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

    Renderer renderer;
    if (!renderer.init(window)) {
        std::fprintf(stderr, "could not start the graphics renderer\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    muuk::desktop::LiveController controller;
    muuk::desktop::DashboardWindow dashboard;
    dashboard.setFonts(bold, large, numbers);
    dashboard.setRendererName(renderer.name());
    muuk::desktop::glass::setLowPower(renderer.software());
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
        // Wakes immediately on any input; otherwise redraws often enough for
        // live prices/P&L to keep moving, but no more than the window needs:
        // an unattended VPS window mostly sits minimized or in the
        // background, and a GPU-less VPS draws every frame on the CPU. The
        // bot itself runs on its own threads and is never slowed by this.
        double fps = renderer.software() ? 15.0 : 60.0;
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            fps = 2.0;  // still ticks auto-reconnect and the exit guard
        } else if (!glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
            fps = renderer.software() ? 5.0 : 10.0;
        }
        glfwWaitEventsTimeout(1.0 / fps);

        // Exit guard: the close button (or Alt+F4) only *asks* to exit; with
        // trades still open the dashboard shows a dialog first.
        if (glfwWindowShouldClose(window)) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            dashboard.requestExit(controller);
            if (dashboard.exitApproved()) break;
        }

        renderer.newFrame();
        ImGui::NewFrame();

        dashboard.render(controller);

        ImGui::Render();
        renderer.present();
    }

    controller.stop();

#ifdef _WIN32
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWndProc));
    g_controller = nullptr;
#endif

    renderer.shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
