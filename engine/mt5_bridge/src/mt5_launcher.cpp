#include "muuk/mt5_bridge/mt5_launcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace muuk::mt5_bridge {

namespace fs = std::filesystem;

namespace {

const unsigned char kEaSource[] = {
#include "muuk_bridge_ea.inc"
};

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

std::wstring normalized(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(p, ec);
    if (ec) abs = p;
    std::wstring s = lower(abs.wstring());
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// MT5's own text files (origin.txt, presets, startup files) are UTF-16LE.
std::wstring readUtf16(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring out;
    std::size_t i = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                            static_cast<unsigned char>(bytes[1]) == 0xFE
                        ? 2
                        : 0;
    for (; i + 1 < bytes.size(); i += 2) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
                                           (static_cast<unsigned char>(bytes[i + 1]) << 8)));
    }
    return out;
}

bool writeUtf16(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.put(static_cast<char>(0xFF));
    out.put(static_cast<char>(0xFE));
    for (const char ch : text) {  // ASCII content only (names, numbers, the password as typed)
        out.put(ch);
        out.put('\0');
    }
    return static_cast<bool>(out);
}

bool isGoldName(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s.rfind("XAUUSD", 0) == 0 || s.rfind("GOLD", 0) == 0;
}

// A chart file's text (MT5 writes them in UTF-16LE, older ones in UTF-8/ANSI).
std::string chartText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::string out;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const unsigned c = static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
            out.push_back(c < 128 ? static_cast<char>(c) : '?');
        }
        return out;
    }
    return bytes;
}

std::string chartSymbolOf(const std::string& text) {
    const auto at = text.find("\nsymbol=");
    const std::size_t start = at == std::string::npos ? (text.rfind("symbol=", 0) == 0 ? 7 : std::string::npos) : at + 8;
    if (start == std::string::npos) return {};
    const auto end = text.find_first_of("\r\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path appDataTerminals() {
#ifdef _WIN32
    wchar_t* appData = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
        fs::path p = fs::path(appData) / "MetaQuotes" / "Terminal";
        free(appData);
        return p;
    }
#endif
    return {};
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\xEF' || s[i] == '\xBB' || s[i] == '\xBF')) ++i;  // UTF-8 BOM
    return s.substr(i);
}

}  // namespace

std::string knownGoldSymbol(const fs::path& dataDir, const std::string& login, const std::string& server) {
    std::error_code ec;
    // 1. What the bridge traded on this account before (it writes this file once connected).
    if (!login.empty()) {
        const std::string sym = trimmed(readFile(dataDir / "MQL5" / "Files" / ("MuukBridge_" + login + ".symbol")));
        if (isGoldName(sym)) return sym;
    }
    // 2. The gold symbol this broker server has sent prices for (bases\<server>\ticks\<symbol>);
    //    a symbol the account can't trade has no ticks.
    if (server.empty()) return {};
    const std::wstring want = lower(fs::path(server).wstring());
    std::string best;
    fs::file_time_type newest{};
    for (const auto& srv : fs::directory_iterator(dataDir / "bases", ec)) {
        if (!srv.is_directory() || lower(srv.path().filename().wstring()) != want) continue;
        for (const auto& sym : fs::directory_iterator(srv.path() / "ticks", ec)) {
            if (!sym.is_directory() || !isGoldName(sym.path().filename().string())) continue;
            for (const auto& f : fs::directory_iterator(sym.path(), ec)) {
                const auto t = f.last_write_time(ec);
                if (!ec && (best.empty() || t > newest)) {
                    newest = t;
                    best = sym.path().filename().string();
                }
            }
        }
    }
    return best;
}

std::string bridgeEaSource() { return std::string(reinterpret_cast<const char*>(kEaSource), sizeof(kEaSource)); }

std::optional<fs::path> dataDirFor(const fs::path& terminal) {
    const fs::path root = appDataTerminals();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return std::nullopt;
    const std::wstring installDir = normalized(terminal.parent_path());
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        const fs::path origin = entry.path() / "origin.txt";
        if (!fs::is_regular_file(origin, ec)) continue;
        std::wstring text = readUtf16(origin);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
        if (normalized(text) == installDir) return entry.path();
    }
    return std::nullopt;
}

std::vector<Mt5Install> findMt5Installs() {
    std::vector<Mt5Install> out;
    std::vector<fs::path> roots;
#ifdef _WIN32
    for (const wchar_t* var : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        wchar_t* v = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&v, &len, var) == 0 && v) {
            roots.emplace_back(v);
            free(v);
        }
    }
#endif
    std::vector<std::wstring> seen;
    std::error_code ec;
    for (const auto& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            const fs::path exe = entry.path() / "terminal64.exe";
            if (!fs::is_regular_file(exe, ec)) continue;
            const std::wstring key = normalized(exe);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            Mt5Install i;
            i.terminal = exe;
            i.label = entry.path().filename().string();
            if (const auto d = dataDirFor(exe)) i.dataDir = *d;
            out.push_back(i);
        }
    }
    return out;
}

bool isMt5Running(const fs::path& terminal) {
#ifdef _WIN32
    const std::wstring want = normalized(terminal);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (lower(pe.szExeFile) != L"terminal64.exe") continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(h, 0, path, &size) && normalized(fs::path(path)) == want) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
#else
    (void)terminal;
    return false;
#endif
}

Mt5StartResult startMt5WithBridge(const Mt5StartOptions& o, const std::function<void(const std::string&)>& log) {
    Mt5StartResult r;
    auto say = [&](const std::string& s) {
        if (log) log("[mt5] " + s);
    };
    fs::path terminal = o.terminal;
    std::error_code ec;
    if (terminal.empty()) {
        const auto installs = findMt5Installs();
        if (installs.empty()) {
            r.message = "MetaTrader 5 isn't installed on this PC -- install your broker's MT5 and log in once.";
            return r;
        }
        terminal = installs.front().terminal;
    }
    if (!fs::is_regular_file(terminal, ec)) {
        r.message = "MetaTrader 5 not found at " + terminal.string() + " (Settings > Account > MT5 terminal).";
        return r;
    }
    const auto data = dataDirFor(terminal);
    if (!data) {
        r.message = "MetaTrader 5 at " + terminal.parent_path().string() +
                    " has never been opened on this Windows user -- open it once (and log in), then connect again.";
        return r;
    }

    // 1. The EA: MuukBridge built into Muuk, compiled by this MT5's own MetaEditor.
    const fs::path eaDir = *data / "MQL5" / "Experts" / "Muuk";
    fs::create_directories(eaDir, ec);
    const fs::path mq5 = eaDir / "MuukBridge.mq5";
    const fs::path ex5 = eaDir / "MuukBridge.ex5";
    const std::string source = bridgeEaSource();
    const bool sourceChanged = readFile(mq5) != source;
    if (sourceChanged) {
        std::ofstream out(mq5, std::ios::binary | std::ios::trunc);
        out << source;
    }
    if (sourceChanged || !fs::is_regular_file(ex5, ec)) {
        const fs::path editor = terminal.parent_path() / "metaeditor64.exe";
        if (!fs::is_regular_file(editor, ec)) {
            r.message = "MetaEditor (metaeditor64.exe) is missing next to MT5 -- can't build MuukBridge.";
            return r;
        }
        say("building MuukBridge with " + editor.string());
#ifdef _WIN32
        fs::remove(ex5, ec);
        const fs::path logFile = eaDir / "MuukBridge.compile.log";
        std::wstring cmd = L"\"" + editor.wstring() + L"\" /compile:\"" + mq5.wstring() + L"\" /log:\"" + logFile.wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
#endif
        if (!fs::is_regular_file(ex5, ec)) {
            r.message = "MuukBridge didn't compile -- see " + (eaDir / "MuukBridge.compile.log").string();
            return r;
        }
    }

    // 2. Its inputs: the pipe to this Muuk.
    const fs::path presets = *data / "MQL5" / "Presets";
    fs::create_directories(presets, ec);
    writeUtf16(presets / "MuukBridge.set", "PipeName=" + o.pipeName + "\r\nTradeSymbol=\r\nMagicNumber=770077\r\n"
                                           "TimerMs=25\r\nSlippagePoints=50\r\n");

    if (isMt5Running(terminal)) {
        r.alreadyRunning = true;
        r.message = "MetaTrader 5 is already open -- the bridge connects if MuukBridge is on a chart there (it stays "
                    "on the chart once Muuk has started MT5 once). If it doesn't connect, close MT5 and connect again.";
        say(r.message);
        return r;
    }

    // 3. The chart: this account's gold symbol under its real name on this
    //    broker (XAUUSD.c, XAUUSDm...). A chart of a gold symbol the account
    //    can't trade never finishes loading ("symbol synchronization timeout")
    //    and the bridge never starts. Known from the bridge's last run on this
    //    account, else the gold symbol the server sent prices for, else a gold
    //    chart MT5 already has. Leftover bridge charts from earlier starts go
    //    (MT5 is closed now, so its profile files can be changed).
    std::string chartSymbol = o.chartSymbol;
    const bool autoChart = chartSymbol.empty() || chartSymbol == "XAUUSD" || chartSymbol == "auto";
    const std::string known = autoChart ? knownGoldSymbol(*data, o.login, o.server) : std::string{};
    fs::file_time_type newest{};
    std::string fromCharts;
    for (const auto& entry : fs::recursive_directory_iterator(*data / "MQL5" / "Profiles" / "Charts", ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".chr") continue;
        const std::string text = chartText(entry.path());
        const std::string sym = chartSymbolOf(text);
        if (autoChart && isGoldName(sym)) {
            const auto t = entry.last_write_time(ec);
            if (fromCharts.empty() || t > newest) {
                newest = t;
                fromCharts = sym;
            }
        }
        if (text.find("MuukBridge") != std::string::npos) {
            fs::remove(entry.path(), ec);  // a bridge chart from an earlier start: the startup file adds a fresh one
        }
    }
    if (autoChart) chartSymbol = !known.empty() ? known : fromCharts;
    if (chartSymbol.empty() || chartSymbol == "auto") chartSymbol = "XAUUSD";
    say("bridge chart: " + chartSymbol);

    // 4. The startup file: log in, algo trading on, MuukBridge on a chart.
    std::ostringstream ini;
    if (!o.login.empty()) {
        ini << "[Common]\r\nLogin=" << o.login << "\r\n";
        if (!o.password.empty()) ini << "Password=" << o.password << "\r\n";
        if (!o.server.empty()) ini << "Server=" << o.server << "\r\n";
        ini << "KeepPrivate=1\r\nNewsEnable=0\r\n";
    }
    ini << "[Experts]\r\nAllowLiveTrading=1\r\nAllowDllImport=0\r\nEnabled=1\r\nAccount=0\r\nProfile=0\r\n";
    ini << "[StartUp]\r\nExpert=Muuk\\MuukBridge\r\nExpertParameters=MuukBridge.set\r\nSymbol=" << chartSymbol
        << "\r\nPeriod=M1\r\n";
    const fs::path startup = *data / "muuk_startup.ini";
    if (!writeUtf16(startup, ini.str())) {
        r.message = "can't write " + startup.string();
        return r;
    }
#ifdef _WIN32
    std::wstring cmd = L"\"" + terminal.wstring() + L"\" /config:\"" + startup.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, terminal.parent_path().wstring().c_str(),
                        &si, &pi)) {
        fs::remove(startup, ec);
        r.message = "couldn't start " + terminal.string() + " (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#endif
    // The file may hold the password: gone once MT5 has read it.
    std::thread([startup] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::error_code e;
        fs::remove(startup, e);
    }).detach();
    r.started = true;
    r.message = "MetaTrader 5 started (minimized) with MuukBridge" +
                (o.login.empty() ? std::string(" on its last account") : " for account " + o.login);
    say(r.message);
    return r;
}

Mt5StartResult restartMt5WithBridge(const Mt5StartOptions& o, const std::function<void(const std::string&)>& log) {
    fs::path terminal = o.terminal;
    if (terminal.empty()) {
        const auto installs = findMt5Installs();
        if (!installs.empty()) terminal = installs.front().terminal;
    }
#ifdef _WIN32
    if (!terminal.empty() && isMt5Running(terminal)) {
        if (log) log("[mt5] closing MetaTrader 5 to start it again with the bridge");
        const std::wstring want = normalized(terminal);
        struct Ctx {
            std::wstring want;
        } ctx{want};
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                const auto* c = reinterpret_cast<const Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!h) return TRUE;
                wchar_t path[MAX_PATH * 2];
                DWORD size = static_cast<DWORD>(std::size(path));
                const bool match = QueryFullProcessImageNameW(h, 0, path, &size) && normalized(fs::path(path)) == c->want;
                CloseHandle(h);
                if (match) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        for (int i = 0; i < 100 && isMt5Running(terminal); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (isMt5Running(terminal)) {
            Mt5StartResult r;
            r.alreadyRunning = true;
            r.message = "MetaTrader 5 didn't close (a dialog open?) -- close it yourself, then press Connect.";
            if (log) log("[mt5] " + r.message);
            return r;
        }
    }
#endif
    Mt5StartOptions again = o;
    again.terminal = terminal;
    return startMt5WithBridge(again, log);
}

}  // namespace muuk::mt5_bridge
