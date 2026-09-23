#pragma once

#include <array>
#include <optional>
#include <string>

#include "live_controller.hpp"

struct ImFont;

namespace muuk::desktop {

// The whole app window: connection bar, live KPI cards, a range panel
// (visual range + Open/Close range + auto-continue + emergency controls),
// a settings panel for tuning the strategy, and tabs listing open
// positions, pending orders and the log -- each position/order with its
// own Close/Cancel button. Holds only UI-local state (text buffers, the
// settings being edited, theme); all live state comes from LiveController
// every frame.
class DashboardWindow {
public:
    // Pre-fills the credentials-path field via findDefaultConfigPath()
    // (config_discovery.hpp) and applies the initial (dark) theme.
    DashboardWindow();

    // Optional extra fonts (loaded by main.cpp); nullptr falls back to the
    // default font.
    void setFonts(ImFont* bold, ImFont* large);

    void render(LiveController& controller);

private:
    enum class Confirm { None, CloseRange, CloseAllPositions, CancelAllPending };

    void applyTheme() const;
    void renderConnectionBar(LiveController& controller, bool running, bool connected);
    void renderKpiCards(const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap, bool connected);
    void renderRangePanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                          bool connected);
    void renderSettingsPanel(LiveController& controller, const std::optional<muuk::ctrader_fix::LiveSnapshot>& snap,
                             bool connected);
    void renderPositionsTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderOrdersTab(LiveController& controller, const muuk::ctrader_fix::LiveSnapshot& snap, bool connected);
    void renderLogTab(LiveController& controller);
    void renderConfirmPopup(LiveController& controller);
    void flash(const std::string& message);

    std::array<char, 512> configPathBuffer_{};
    bool darkMode_ = true;

    muuk::ctrader_fix::LiveSettings editSettings_;
    bool settingsLoaded_ = false;  // editSettings_ holds real values (from the file or the bot)
    bool settingsDirty_ = false;   // edited in the UI but not yet applied/saved

    std::array<char, 128> logFilter_{};
    bool logAutoScroll_ = true;

    Confirm confirm_ = Confirm::None;
    bool openConfirmRequested_ = false;

    std::string flashMessage_;
    double flashUntil_ = 0.0;

    ImFont* boldFont_ = nullptr;
    ImFont* largeFont_ = nullptr;
};

}  // namespace muuk::desktop
