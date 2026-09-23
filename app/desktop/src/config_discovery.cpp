#include "config_discovery.hpp"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace muuk::desktop {

namespace {

namespace fs = std::filesystem;

bool isLocalJsonConfig(const fs::path& path) {
    const std::string name = path.filename().string();
    const std::string suffix = ".local.json";
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<fs::path> exeDirectory() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return std::nullopt;
    return fs::path(buffer).parent_path();
#else
    return std::nullopt;
#endif
}

std::optional<fs::path> searchUpward(fs::path dir, int maxLevels) {
    for (int i = 0; i < maxLevels; ++i) {
        std::error_code ec;
        if (fs::exists(dir, ec) && !ec) {
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && isLocalJsonConfig(entry.path())) {
                    return entry.path();
                }
            }
        }
        const fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> findDefaultConfigPath() {
    constexpr int kMaxLevels = 8;

    if (const auto exeDir = exeDirectory()) {
        if (const auto found = searchUpward(*exeDir, kMaxLevels)) {
            return found->string();
        }
    }

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        if (const auto found = searchUpward(cwd, kMaxLevels)) {
            return found->string();
        }
    }

    return std::nullopt;
}

}  // namespace muuk::desktop
