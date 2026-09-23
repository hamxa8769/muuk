#include "open_path.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace muuk::desktop {

bool openPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;  // ShellExecute's documented success threshold
#else
    (void)path;
    return false;
#endif
}

}  // namespace muuk::desktop
