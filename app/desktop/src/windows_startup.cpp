#include "windows_startup.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
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
namespace {
std::filesystem::path shortcutPath(ShortcutPlace where) {
    PWSTR dir = nullptr;
    const KNOWNFOLDERID& id = where == ShortcutPlace::Desktop ? FOLDERID_Desktop : FOLDERID_Programs;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) return {};
    std::filesystem::path p = std::filesystem::path(dir) / L"Muuk.lnk";
    CoTaskMemFree(dir);
    return p;
}
}  // namespace

bool hasShortcut(ShortcutPlace where) {
    const auto p = shortcutPath(where);
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec);
}

bool createShortcut(ShortcutPlace where) {
    const auto path = shortcutPath(where);
    if (path.empty()) return false;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        const std::wstring exe = thisExe();
        link->SetPath(exe.c_str());
        link->SetWorkingDirectory(std::filesystem::path(exe).parent_path().c_str());
        link->SetIconLocation(exe.c_str(), 0);
        link->SetDescription(L"Muuk -- XAUUSD range bot");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            ok = SUCCEEDED(file->Save(path.c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
#else
bool startsWithWindows() { return false; }
bool setStartWithWindows(bool) { return false; }
bool hasShortcut(ShortcutPlace) { return false; }
bool createShortcut(ShortcutPlace) { return false; }
#endif

}  // namespace muuk::desktop
