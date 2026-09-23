#pragma once

#include <array>

#include "live_controller.hpp"

namespace muuk::desktop {

// Renders the single-window dashboard (proposal §9's status table: symbol,
// strategy status, range, order count, basket P/L, spread) plus Start/Stop
// controls, a light/dark theme toggle, and a scrolling log, from one
// LiveController. Holds only UI-local state (the credentials-path text
// buffer, theme choice) — all actual state comes from LiveController each
// frame.
class DashboardWindow {
public:
    // Pre-fills the credentials-path field via findDefaultConfigPath()
    // (config_discovery.hpp) and applies the initial (dark) theme.
    DashboardWindow();

    void render(LiveController& controller);

private:
    void applyTheme() const;

    std::array<char, 512> configPathBuffer_{};
    bool darkMode_ = true;
};

}  // namespace muuk::desktop
