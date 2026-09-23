#pragma once

#include <filesystem>

namespace muuk::desktop {

// Opens a file (with its default app, e.g. a PDF viewer) or a folder (in
// Explorer). Returns false if the OS refused. No-op returning false off Windows.
bool openPath(const std::filesystem::path& path);

}  // namespace muuk::desktop
