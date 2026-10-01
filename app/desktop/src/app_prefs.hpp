#pragma once

#include <filesystem>
#include <string>

namespace muuk::desktop {

// The app's own preferences -- how it looked and what it was pointed at
// last time -- so it opens the way it was left. Trading settings live in
// the config file itself (see LiveController::saveSettings), not here.
struct AppPrefs {
    std::string configPath;    // last config used
    bool darkMode = true;
    bool autoConnect = false;  // connect as soon as the app opens
    bool autoReconnect = true; // reconnect by itself after a dropped connection
    bool liteMode = false;     // least CPU (always on without a graphics card)
    // Who runs this copy of the app. One user today -- the owner, as admin;
    // the role is kept so other users (with their own accounts) can come later.
    std::string userName;      // empty = this computer's user name
    std::string role = "admin";
};

// The name shown for the user: prefs.userName, else this computer's user name.
[[nodiscard]] std::string displayUserName(const AppPrefs& prefs);

// %APPDATA%\Muuk\desktop.json (or next to the working directory elsewhere).
[[nodiscard]] std::filesystem::path prefsFile();
// Defaults if the file is missing or unreadable.
[[nodiscard]] AppPrefs loadPrefs();
// Best effort: a failure to save preferences never interrupts the app.
void savePrefs(const AppPrefs& prefs);

}  // namespace muuk::desktop
