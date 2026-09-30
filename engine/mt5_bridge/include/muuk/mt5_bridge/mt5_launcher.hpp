#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace muuk::mt5_bridge {

// A MetaTrader 5 installed on this PC.
struct Mt5Install {
    std::filesystem::path terminal;  // ...\terminal64.exe
    std::filesystem::path dataDir;   // %APPDATA%\MetaQuotes\Terminal\<id> (empty until MT5 has run once)
    std::string label;               // the install folder's name, e.g. "JustMarkets MetaTrader 5"
};

// Every terminal64.exe under Program Files (one folder deep), with its data folder.
[[nodiscard]] std::vector<Mt5Install> findMt5Installs();
// The data folder MT5 uses for this terminal (its origin.txt names the install folder).
[[nodiscard]] std::optional<std::filesystem::path> dataDirFor(const std::filesystem::path& terminal);
[[nodiscard]] bool isMt5Running(const std::filesystem::path& terminal);

struct Mt5StartOptions {
    std::filesystem::path terminal;  // empty = the first one found
    std::string login;               // empty = whatever account MT5 last used
    std::string password;            // only with login; written to a startup file that is deleted again
    std::string server;
    std::string chartSymbol = "XAUUSD";  // the chart the bridge is attached to (it finds the gold symbol itself)
    std::string pipeName = "MuukBridge_5200";
};

struct Mt5StartResult {
    bool started = false;         // MT5 was launched with the bridge
    bool alreadyRunning = false;  // MT5 was open already: the bridge connects if it's still on a chart
    std::string message;          // what happened, for the log / UI
};

// Gets MetaTrader 5 ready for Muuk with nothing to do by hand: writes the
// MuukBridge EA (built into Muuk) into MT5's data folder and compiles it
// with MT5's own MetaEditor, writes its input preset (the pipe name), and
// starts MT5 minimized with a startup file that logs in (if a login is
// given), turns algo trading on and attaches MuukBridge to a chart. MT5
// remembers that chart, so later starts run the bridge by themselves.
// Blocking (compiling takes a few seconds): call it off the UI thread.
[[nodiscard]] Mt5StartResult startMt5WithBridge(const Mt5StartOptions& options,
                                                const std::function<void(const std::string&)>& log);

// Closes a running MetaTrader 5 the way its own X button does (it saves its
// charts), waits for it to exit, then startMt5WithBridge. Never kills it: if
// it doesn't close within 20 s (e.g. a dialog is open), says so.
[[nodiscard]] Mt5StartResult restartMt5WithBridge(const Mt5StartOptions& options,
                                                  const std::function<void(const std::string&)>& log);

// The MuukBridge.mq5 source built into this program.
[[nodiscard]] std::string bridgeEaSource();

}  // namespace muuk::mt5_bridge
