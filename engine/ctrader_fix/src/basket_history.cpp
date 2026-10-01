#include "muuk/ctrader_fix/basket_history.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace muuk::ctrader_fix {

BasketVerdict basketVerdict(const BasketRecord& b) {
    BasketVerdict v;
    auto money = [](double x) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.2f", x < -0.005 ? "-" : (x > 0.005 ? "+" : ""), std::fabs(x));
        return std::string(buf);
    };
    // A second fill means the price turned against the first position: the
    // basket was in loss on its way, whatever it ended at.
    const bool wentIntoLoss = b.legs.size() >= 2;
    const bool guarded = b.guardHeld || b.locked || b.escapeTries > 0;
    const bool up = b.net > 0.005;
    const bool down = b.net < -0.005;

    std::string detail;
    auto add = [&](const std::string& s) { detail += (detail.empty() ? "" : ", ") + s; };
    if (b.guardHeld) add("margin guard held a level");
    if (b.locked) add("locked at " + money(b.lockedLoss));
    if (b.escapeTries > 0) add(std::to_string(b.escapeTries) + (b.escapeTries == 1 ? " escape try" : " escape tries"));
    if (b.breakEvenFromLevel > 0) add("break-even target from level " + std::to_string(b.breakEvenFromLevel));
    if (wentIntoLoss && b.worstNet < -0.005) add("deepest " + money(b.worstNet));
    v.detail = detail;

    const std::string& o = b.outcome;
    auto bySign = [&](const std::string& how) {
        v.kind = up ? (wentIntoLoss || guarded ? BasketVerdictKind::Recovered : BasketVerdictKind::Profit)
                    : (down ? BasketVerdictKind::Loss : BasketVerdictKind::Neutral);
        v.label = how + (up ? " - in profit" : (down ? " - in loss" : " - at 0"));
    };
    if (o == "target") {
        if (guarded) {
            v.kind = BasketVerdictKind::Recovered;
            v.label = "Guard used -> closed in profit";
        } else if (b.breakEvenFromLevel > 0) {
            v.kind = BasketVerdictKind::Recovered;
            v.label = "Loss recovered -> break-even exit";
        } else if (wentIntoLoss) {
            v.kind = BasketVerdictKind::Recovered;
            v.label = "Loss recovered -> profit target";
        } else {
            v.kind = BasketVerdictKind::Profit;
            v.label = "Profit target";
        }
    } else if (o == "break-even") {
        v.kind = down ? BasketVerdictKind::Loss : BasketVerdictKind::Recovered;
        v.label = b.locked ? "Guard: lock -> back to break-even" : "Guard held a level -> break-even exit";
    } else if (o == "stop loss") {
        v.kind = BasketVerdictKind::Loss;
        v.label = "Stop loss - closed in loss";
    } else if (o == "locked") {
        bySign("Locked, then closed");
    } else if (o == "daily limit") {
        bySign("Daily loss limit");
    } else if (o == "manual") {
        bySign("Closed by you");
    } else if (o == "market pause") {
        bySign("Closed for a market pause");
    } else if (o == "guard") {
        bySign("Closed when the app exited");
    } else if (o == "interrupted") {
        bySign("Interrupted");
    } else if (o == "audit") {
        bySign("Closed by the audit");
    } else if (o == "failed") {
        v.kind = BasketVerdictKind::Loss;
        v.label = "A close was rejected";
    } else {
        bySign("Closed");
    }
    return v;
}

nlohmann::json basketToJson(const BasketRecord& b) {
    nlohmann::json legs = nlohmann::json::array();
    for (const auto& l : b.legs) {
        legs.push_back({{"id", l.id},
                        {"positionId", l.positionId},
                        {"side", l.buy ? "buy" : "sell"},
                        {"lots", l.lots},
                        {"requestedPrice", l.requestedPrice},
                        {"entryPrice", l.entryPrice},
                        {"exitPrice", l.exitPrice},
                        {"gross", l.gross},
                        {"commission", l.commission},
                        {"net", l.net},
                        {"openedAt", l.openedAt},
                        {"closedAt", l.closedAt},
                        {"adopted", l.adopted},
                        {"note", l.note}});
    }
    return {{"number", b.number},
            {"session", b.session},
            {"symbol", b.symbol},
            {"startedAt", b.startedAt},
            {"closedAt", b.closedAt},
            {"durationSeconds", b.durationSeconds},
            {"contractSize", b.contractSize},
            {"rangeLower", b.rangeLower},
            {"rangeUpper", b.rangeUpper},
            {"closeReason", b.closeReason},
            {"outcome", b.outcome},
            {"ordersPlaced", b.ordersPlaced},
            {"ordersFilled", b.ordersFilled},
            {"ordersRejected", b.ordersRejected},
            {"ordersMissed", b.ordersMissed},
            {"worstNet", b.worstNet},
            {"bestNet", b.bestNet},
            {"maxOpenLots", b.maxOpenLots},
            {"netAtTrigger", b.netAtTrigger},
            {"guardHeld", b.guardHeld},
            {"locked", b.locked},
            {"lockedLoss", b.lockedLoss},
            {"escapeTries", b.escapeTries},
            {"breakEvenFromLevel", b.breakEvenFromLevel},
            {"marketSession", b.marketSession},
            {"gross", b.gross},
            {"commission", b.commission},
            {"net", b.net},
            {"legs", legs}};
}

BasketRecord basketFromJson(const nlohmann::json& j) {
    BasketRecord b;
    b.number = j.value("number", 0);
    b.session = j.value("session", std::string());
    b.symbol = j.value("symbol", std::string());
    b.startedAt = j.value("startedAt", std::string());
    b.closedAt = j.value("closedAt", std::string());
    b.durationSeconds = j.value("durationSeconds", 0.0);
    b.contractSize = j.value("contractSize", 100.0);
    b.rangeLower = j.value("rangeLower", 0.0);
    b.rangeUpper = j.value("rangeUpper", 0.0);
    b.closeReason = j.value("closeReason", std::string());
    b.outcome = j.value("outcome", std::string());
    b.ordersPlaced = j.value("ordersPlaced", 0);
    b.ordersFilled = j.value("ordersFilled", 0);
    b.ordersRejected = j.value("ordersRejected", 0);
    b.ordersMissed = j.value("ordersMissed", 0);
    b.worstNet = j.value("worstNet", 0.0);
    b.bestNet = j.value("bestNet", 0.0);
    b.maxOpenLots = j.value("maxOpenLots", 0.0);
    b.netAtTrigger = j.value("netAtTrigger", 0.0);
    b.guardHeld = j.value("guardHeld", false);
    b.locked = j.value("locked", false);
    b.lockedLoss = j.value("lockedLoss", 0.0);
    b.escapeTries = j.value("escapeTries", 0);
    b.breakEvenFromLevel = j.value("breakEvenFromLevel", 0);
    b.marketSession = j.value("marketSession", std::string());
    b.gross = j.value("gross", 0.0);
    b.commission = j.value("commission", 0.0);
    b.net = j.value("net", 0.0);
    if (j.contains("legs")) {
        for (const auto& lj : j.at("legs")) {
            BasketLegRecord l;
            l.id = lj.value("id", uint64_t{0});
            l.positionId = lj.value("positionId", std::string());
            l.buy = lj.value("side", std::string("buy")) == "buy";
            l.lots = lj.value("lots", 0.0);
            l.requestedPrice = lj.value("requestedPrice", 0.0);
            l.entryPrice = lj.value("entryPrice", 0.0);
            l.exitPrice = lj.value("exitPrice", 0.0);
            l.gross = lj.value("gross", 0.0);
            l.commission = lj.value("commission", 0.0);
            l.net = lj.value("net", 0.0);
            l.openedAt = lj.value("openedAt", std::string());
            l.closedAt = lj.value("closedAt", std::string());
            l.adopted = lj.value("adopted", false);
            l.note = lj.value("note", std::string());
            b.legs.push_back(l);
        }
    }
    return b;
}

void appendBasketHistory(const std::filesystem::path& file, const BasketRecord& basket) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::app);
    if (!out) throw std::runtime_error("cannot write basket history: " + file.string());
    out << basketToJson(basket).dump() << "\n";
}

std::vector<BasketRecord> loadBasketHistory(const std::filesystem::path& file) {
    std::vector<BasketRecord> out;
    std::ifstream in(file);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            out.push_back(basketFromJson(nlohmann::json::parse(line)));
        } catch (const std::exception&) {
            // A torn last line (e.g. power loss mid-write) -- skip it, keep the rest.
        }
    }
    return out;
}

std::string localTimestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

}  // namespace muuk::ctrader_fix
