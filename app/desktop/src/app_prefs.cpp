#include "app_prefs.hpp"

#include <cstdlib>
#include <fstream>

#include <nlohmann/json.hpp>

namespace muuk::desktop {

std::filesystem::path prefsFile() {
#ifdef _WIN32
    wchar_t* appData = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData != nullptr) {
        const std::filesystem::path dir = std::filesystem::path(appData) / "Muuk";
        std::free(appData);
        return dir / "desktop.json";
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec) / "muuk_desktop.json";
}

AppPrefs loadPrefs() {
    AppPrefs prefs;
    try {
        std::ifstream in(prefsFile());
        if (!in) return prefs;
        const nlohmann::json j = nlohmann::json::parse(in);
        prefs.configPath = j.value("configPath", std::string());
        prefs.darkMode = j.value("darkMode", true);
        prefs.autoConnect = j.value("autoConnect", false);
        prefs.autoReconnect = j.value("autoReconnect", true);
    } catch (const std::exception&) {
        return AppPrefs{};
    }
    return prefs;
}

void savePrefs(const AppPrefs& prefs) {
    try {
        const std::filesystem::path file = prefsFile();
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::trunc);
        out << nlohmann::json{{"configPath", prefs.configPath},
                              {"darkMode", prefs.darkMode},
                              {"autoConnect", prefs.autoConnect},
                              {"autoReconnect", prefs.autoReconnect}}
                   .dump(2);
    } catch (const std::exception&) {
        // Preferences are a convenience -- never worth interrupting the app for.
    }
}

}  // namespace muuk::desktop
