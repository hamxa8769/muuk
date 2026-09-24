#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app_prefs.hpp"
#include "live_controller.hpp"
#include "system_monitor.hpp"
#include "muuk/ctrader_fix/volume_stats.hpp"

struct ImFont;

namespace muuk::desktop {

// The whole app window: header (connection), live KPI cards, a range panel
// (live range chart + Open/Close range + auto-continue + emergency
// controls), a settings panel for tuning the strategy, and tabs listing
// open positions, pending orders and the log -- each position/order with
// its own Close/Cancel button. Drawn in a frosted-glass style (glass_ui.hpp).
// Holds only UI-local state (text buffers, the settings being edited,
// theme); all live state comes from LiveController every frame.
class DashboardWindow {
public:
    // Opens the way it was left (app_prefs.hpp): the last config file and
    // theme; falls back to findDefaultConfigPath() (config_discovery.hpp).
    DashboardWindow();

    // "Connect when the app opens" (saved preference) -- main.cpp acts on it.
    [[nodiscard]] bool autoConnect() const { return prefs_.autoConnect; }

    // Optional extra fonts (loaded by main.cpp); nullptr falls back to the
    // default font.
    void setFonts(ImFont* bold, ImFont* large, ImFont* numbers);

    // Called whenever the operator switches dark/light, e.g. so main.cpp
    // can match the native window title bar.
    void setThemeChangedCallback(std::function<void(bool dark)> callback);

    // Overrides the config path found by findDefaultConfigPath() (e.g.
    // from the command line).
    void setConfigPath(const std::string& path);
    // Shown in the Performance tab (e.g. "Direct3D 11 software (no GPU)").
    // A software renderer forces Lite mode on.
    void setRenderer(std::string name, bool software) {
        rendererName_ = std::move(name);
        softwareRenderer_ = software;
    }
    // Lite mode: flat look, no animations, and main.cpp redraws only when
    // something happens (1 frame/s idle). Always on without a GPU.
    [[nodiscard]] bool liteMode() const { return prefs_.liteMode || softwareRenderer_; }
    // For frames that aren't drawn (window hidden): keeps auto-reconnect going.
    void tickWithoutDrawing(LiveController& controller);
    [[nodiscard]] std::string configPath() const { return configPathBuffer_.data(); }

    void render(LiveController& controller);

    // Exit guard. main.cpp calls requestExit() instead of closing when the
    // window's close button is pressed: with nothing open at the broker the
    // exit is approved straight away; otherwise a dialog asks whether to
    // close everything first (and waits until the account is flat), leave
    // the positions open, or stay. The app quits once exitApproved().
    void requestExit(LiveController& controller);
    [[nodiscard]] bool exitApproved() const { return exitApproved_; }

private:
    enum class Confirm { None, CloseRange, CloseAllPositions, CancelAllPending, ResetData, PlaceNow };
    // The tabs on the right; a KPI card click selects one.
    enum class Tab { None, Positions, Orders, History, Volume, Performance, Settings, Log };
    [[nodiscard]] int tabFlags(Tab tab) const;

    void setTheme(bool dark);
    void renderHeader(LiveController& controller, bool running, bool connected);
    void renderKpiCards(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap, bool connected);
    void renderRangePanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                          bool connected);
    void renderSettingsPanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                             bool connected);
    void renderPositionsTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderOrdersTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderLogTab(LiveController& controller);
    void renderHistoryTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void renderSpeed(const muuk::ctrader_fix::LiveSnapshot& snap);
    void renderVolumeTab();
    void renderPerformanceTab(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void samplePerformance(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void refreshVolume();
    void renderConfirmPopup(LiveController& controller);
    void renderExitPopup(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void flash(const std::string& message);

    std::array<char, 512> configPathBuffer_{};
    AppPrefs prefs_;
    bool darkMode_ = true;
    std::string dismissedResumeNote_;
    std::function<void(bool)> onThemeChanged_;

    muuk::ctrader_fix::LiveSettings editSettings_;
    bool settingsLoaded_ = false;  // editSettings_ holds real values (from the file or the bot)
    bool settingsDirty_ = false;   // edited in the UI but not yet applied/saved

    std::array<char, 128> logFilter_{};
    bool logAutoScroll_ = true;

    // Cached copy of controller.history(), refreshed when its revision changes.
    std::vector<muuk::ctrader_fix::BasketRecord> history_;
    uint64_t historyRevision_ = ~uint64_t{0};  // forces the first copy
    std::string historyPath_;  // config path the history was loaded for

    // Volume tab, recomputed when the history changes (or the day might have).
    int volumePeriod_ = 0;  // 0 day, 1 week, 2 month
    uint64_t volumeRevision_ = ~uint64_t{0};
    double volumeComputedAt_ = -1000.0;
    std::vector<muuk::ctrader_fix::VolumeBucket> volumeRows_[3];
    muuk::ctrader_fix::VolumeBucket volumeTotal_;
    std::string nowDay_, nowWeek_, nowMonth_;

    bool exitApproved_ = false;
    bool openExitRequested_ = false;
    bool exitClosing_ = false;      // "Close everything & exit" in progress
    double exitClosingSince_ = 0.0;

    Tab selectTab_ = Tab::None;  // switch to this tab on the next frame

    // Performance tab.
    SystemMonitor monitor_;
    std::string rendererName_;
    bool softwareRenderer_ = false;
    double perfSampledAt_ = -1.0;
    uint64_t lastTicks_ = 0;
    double quotesPerSecond_ = 0.0;
    double lastQuoteSeenAt_ = -1.0;
    std::deque<float> rttHistory_;
    std::deque<float> quoteHistory_;
    muuk::core::OrderId placeNowId_ = muuk::core::kInvalidOrderId;  // waiting order awaiting "Place now" confirmation

    Confirm confirm_ = Confirm::None;
    std::string manualReopenText_;
    bool openConfirmRequested_ = false;

    std::string flashMessage_;
    double flashUntil_ = 0.0;

    ImFont* boldFont_ = nullptr;
    ImFont* largeFont_ = nullptr;
    ImFont* numberFont_ = nullptr;
};

}  // namespace muuk::desktop
