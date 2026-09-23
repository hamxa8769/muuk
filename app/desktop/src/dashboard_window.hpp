#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "live_controller.hpp"

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
    // Pre-fills the credentials-path field via findDefaultConfigPath()
    // (config_discovery.hpp) and applies the initial (dark) theme.
    DashboardWindow();

    // Optional extra fonts (loaded by main.cpp); nullptr falls back to the
    // default font.
    void setFonts(ImFont* bold, ImFont* large, ImFont* numbers);

    // Called whenever the operator switches dark/light, e.g. so main.cpp
    // can match the native window title bar.
    void setThemeChangedCallback(std::function<void(bool dark)> callback);

    // Overrides the config path found by findDefaultConfigPath() (e.g.
    // from the command line).
    void setConfigPath(const std::string& path);
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
    enum class Confirm { None, CloseRange, CloseAllPositions, CancelAllPending };

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
    void renderConfirmPopup(LiveController& controller);
    void renderExitPopup(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap);
    void flash(const std::string& message);

    std::array<char, 512> configPathBuffer_{};
    bool darkMode_ = true;
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

    bool exitApproved_ = false;
    bool openExitRequested_ = false;
    bool exitClosing_ = false;      // "Close everything & exit" in progress
    double exitClosingSince_ = 0.0;

    Confirm confirm_ = Confirm::None;
    bool openConfirmRequested_ = false;

    std::string flashMessage_;
    double flashUntil_ = 0.0;

    ImFont* boldFont_ = nullptr;
    ImFont* largeFont_ = nullptr;
    ImFont* numberFont_ = nullptr;
};

}  // namespace muuk::desktop
