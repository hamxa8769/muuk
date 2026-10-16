#include "windows_startup.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#endif

namespace muuk::desktop {

#ifdef _WIN32
namespace {
constexpr const wchar_t* kRunKey = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr const wchar_t* kValue = L"Muuk";

std::wstring thisExe() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return std::wstring(path, n);
}
}  // namespace

bool startsWithWindows() {
    wchar_t value[MAX_PATH * 2 + 4];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
        return false;
    }
    // Ours only if it starts this very program (not an old copy elsewhere).
    return std::wstring(value).find(thisExe()) != std::wstring::npos;
}

bool setStartWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS rc;
    if (on) {
        const std::wstring cmd = L"\"" + thisExe() + L"\"";
        rc = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}
#else
bool startsWithWindows() { return false; }
bool setStartWithWindows(bool) { return false; }
#endif

}  // namespace muuk::desktop
