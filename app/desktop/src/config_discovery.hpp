#pragma once

#include <optional>
#include <string>

namespace muuk::desktop {

// Searches for a *.local.json credentials file (see
// engine/ctrader_fix/config.example.json / docs/ctrader-fix.md), starting
// from the executable's own directory and the current working directory,
// walking upward a few levels each. The exe typically lives several
// levels below the repo root (build/app/desktop/Release/), and the file
// itself typically sits at the repo root, so an upward search from either
// starting point finds it regardless of where the exe is launched from.
// Returns the first match found, or nullopt.
[[nodiscard]] std::optional<std::string> findDefaultConfigPath();

}  // namespace muuk::desktop
