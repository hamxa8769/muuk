#pragma once

#include <array>

#include "live_controller.hpp"

namespace muuk::desktop {

// Renders the single-window dashboard (proposal §9's status table: symbol,
// strategy status, range, order count, basket P/L, spread) plus Start/Stop
// controls and a scrolling log, from one LiveController. Holds only
// UI-local state (the credentials-path text buffer) — all actual state
// comes from LiveController each frame.
class DashboardWindow {
public:
    void render(LiveController& controller);

private:
    std::array<char, 512> configPathBuffer_{};
};

}  // namespace muuk::desktop
