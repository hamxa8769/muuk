#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace muuk::desktop {

// The app's own preferences -- how it looked and what it was pointed at
// last time -- so it opens the way it was left. Trading settings live in
// the config file itself (see LiveController::saveSettings), not here.
// An MT5 account the bridge has been logged in to on this PC (never a password).
struct Mt5KnownAccount {
    std::string login;
    std::string server;
    std::string company;
    std::string currency;  // "USC" = cent
    std::string mode;      // "demo", "real", "contest"
    std::string lastSeen;  // local time
};

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
    // MT5 accounts seen through the bridge, newest first: the MT5 accounts
    // list, and the server for a config that names only the login.
    std::vector<Mt5KnownAccount> mt5Accounts;
    // MT5 logged in to another account than the config's: Muuk switches it.
    bool mt5AutoSwitch = true;
    // Backtest data: the symbols MT5 exports once a day (MQL5\Files\MuukHistory).
    std::string historySymbols = "XAUUSD,XAGUSD,US30,NAS100,USTEC,GER40,DE40,BTCUSD,ETHUSD,EURUSD,GBPUSD,USDJPY";
    std::string historyExportedOn;  // local date of the last export
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
