#include "muuk/ctrader_fix/live_state.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace muuk::ctrader_fix {

using muuk::core::EngineState;
using muuk::core::EngineStatus;
using muuk::core::OrderSide;
using muuk::core::Price;
using muuk::core::Volume;

namespace {

const char* statusName(EngineStatus s) {
    switch (s) {
        case EngineStatus::Active: return "active";
        case EngineStatus::HaltedByRisk: return "halted";
        case EngineStatus::Completed: return "completed";
        case EngineStatus::Idle:
        default: return "idle";
    }
}

EngineStatus statusFromName(const std::string& s) {
    if (s == "active") return EngineStatus::Active;
    if (s == "halted") return EngineStatus::HaltedByRisk;
    if (s == "completed") return EngineStatus::Completed;
    if (s == "idle") return EngineStatus::Idle;
    throw std::runtime_error("state file: unknown engine status '" + s + "'");
}

nlohmann::json legToJson(const PersistedLeg& l) {
    return {{"id", l.id},
            {"side", l.buy ? "buy" : "sell"},
            {"volumeSteps", l.volumeSteps},
            {"priceTicks", l.priceTicks},
            {"requestedPriceTicks", l.requestedPriceTicks},
            {"positionId", l.positionId},
            {"adopted", l.adopted},
            {"closeClOrdId", l.closeClOrdId}};
}

PersistedLeg legFromJson(const nlohmann::json& j) {
    PersistedLeg l;
    l.id = j.at("id").get<uint64_t>();
    l.buy = j.at("side").get<std::string>() == "buy";
    l.volumeSteps = j.at("volumeSteps").get<int64_t>();
    l.priceTicks = j.at("priceTicks").get<int64_t>();
    l.requestedPriceTicks = j.value("requestedPriceTicks", l.priceTicks);
    l.positionId = j.value("positionId", std::string());
    l.adopted = j.value("adopted", false);
    l.closeClOrdId = j.value("closeClOrdId", std::string());
    return l;
}

nlohmann::json legsToJson(const std::vector<PersistedLeg>& legs) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& l : legs) a.push_back(legToJson(l));
    return a;
}

std::vector<PersistedLeg> legsFromJson(const nlohmann::json& j, const char* key) {
    std::vector<PersistedLeg> out;
    if (j.contains(key)) {
        for (const auto& lj : j.at(key)) out.push_back(legFromJson(lj));
    }
    return out;
}

nlohmann::json totalsToJson(const LatencyTracker::Totals& t) {
    return {{"count", t.count}, {"sum", t.sum}, {"max", t.max}};
}

LatencyTracker::Totals totalsFromJson(const nlohmann::json& j, const char* key) {
    LatencyTracker::Totals t;
    if (!j.contains(key)) return t;
    const auto& o = j.at(key);
    t.count = o.value("count", uint64_t{0});
    t.sum = o.value("sum", 0.0);
    t.max = o.value("max", 0.0);
    return t;
}

nlohmann::json engineToJson(const EngineState& e) {
    nlohmann::json orders = nlohmann::json::array();
    for (const auto& a : e.pendingOrders) {
        orders.push_back({{"id", a.order.id},
                          {"sequenceIndex", a.order.sequenceIndex},
                          {"side", a.order.side == OrderSide::BuyStop ? "buy" : "sell"},
                          {"priceTicks", a.order.price.ticks},
                          {"volumeSteps", a.order.volume.steps},
                          {"retryCount", a.retryCount}});
    }
    return {{"status", statusName(e.status)},
            {"rangeLowerTicks", e.range.lowerBound.ticks},
            {"rangeUpperTicks", e.range.upperBound.ticks},
            {"pendingOrders", orders},
            {"buyVolumeSteps", e.buyVolumeSteps},
            {"buySumEntryPriceTicksVolume", e.buySumEntryPriceTicksVolume},
            {"sellVolumeSteps", e.sellVolumeSteps},
            {"sellSumEntryPriceTicksVolume", e.sellSumEntryPriceTicksVolume},
            {"nextSequenceIndex", e.nextSequenceIndex},
            {"nextOrderId", e.nextOrderId},
            {"seeding", e.seeding},
            {"slippageMoneyAccrued", e.slippageMoneyAccrued},
            {"swapMoneyAccrued", e.swapMoneyAccrued},
            {"commissionAccrued", e.commissionAccrued},
            {"filledLevel", e.filledLevel}};
}

EngineState engineFromJson(const nlohmann::json& j) {
    EngineState e;
    e.status = statusFromName(j.at("status").get<std::string>());
    e.range.lowerBound = Price{j.at("rangeLowerTicks").get<int64_t>()};
    e.range.upperBound = Price{j.at("rangeUpperTicks").get<int64_t>()};
    for (const auto& oj : j.at("pendingOrders")) {
        if (e.pendingOrders.size() >= e.pendingOrders.capacity()) {
            throw std::runtime_error("state file: too many pending orders");
        }
        EngineState::Armed a;
        a.order.id = oj.at("id").get<uint64_t>();
        a.order.sequenceIndex = oj.at("sequenceIndex").get<int>();
        a.order.side = oj.at("side").get<std::string>() == "buy" ? OrderSide::BuyStop : OrderSide::SellStop;
        a.order.price = Price{oj.at("priceTicks").get<int64_t>()};
        a.order.volume = Volume{oj.at("volumeSteps").get<int64_t>()};
        a.retryCount = oj.value("retryCount", 0);
        e.pendingOrders.push_back(a);
    }
    e.buyVolumeSteps = j.value("buyVolumeSteps", int64_t{0});
    e.buySumEntryPriceTicksVolume = j.value("buySumEntryPriceTicksVolume", int64_t{0});
    e.sellVolumeSteps = j.value("sellVolumeSteps", int64_t{0});
    e.sellSumEntryPriceTicksVolume = j.value("sellSumEntryPriceTicksVolume", int64_t{0});
    e.nextSequenceIndex = j.value("nextSequenceIndex", 1);
    e.nextOrderId = j.value("nextOrderId", uint64_t{1});
    e.seeding = j.value("seeding", false);
    e.slippageMoneyAccrued = j.value("slippageMoneyAccrued", 0.0);
    e.swapMoneyAccrued = j.value("swapMoneyAccrued", 0.0);
    e.commissionAccrued = j.value("commissionAccrued", -1.0);
    e.filledLevel = j.value("filledLevel", -1);
    return e;
}

}  // namespace

nlohmann::json stateToJson(const PersistedState& s) {
    nlohmann::json j = {{"version", PersistedState::kVersion},
                        {"account", s.account},
                        {"symbol", s.symbol},
                        {"savedAt", s.savedAt},
                        {"engine", engineToJson(s.engine)},
                        {"openLegs", legsToJson(s.openLegs)},
                        {"pendingOrders", legsToJson(s.pendingOrders)},
                        {"cancellingOrders", legsToJson(s.cancellingOrders)},
                        {"pendingCloses", legsToJson(s.pendingCloses)},
                        {"waitingOrders", legsToJson(s.waitingOrders)},
                        {"closeFailed", s.closeFailed},
                        {"closeReason", s.closeReason},
                        {"basketCounter", s.basketCounter},
                        {"nextCancelReqId", s.nextCancelReqId},
                        {"nextCloseReqId", s.nextCloseReqId},
                        {"nextAdoptedId", s.nextAdoptedId},
                        {"statsSince", s.statsSince},
                        {"dayKey", s.dayKey},
                        {"dayRealized", s.dayRealized},
                        {"dailyLimitHit", s.dailyLimitHit},
                        {"locked", s.locked},
                        {"lockedLoss", s.lockedLoss},
                        {"lockedAt", s.lockedAt},
                        {"ticksProcessed", s.ticksProcessed},
                        {"tickDecisionUs", totalsToJson(s.tickDecisionUs)},
                        {"fillReactionUs", totalsToJson(s.fillReactionUs)},
                        {"brokerRoundTripMs", totalsToJson(s.brokerRoundTripMs)}};
    j["currentBasket"] = s.currentBasket ? basketToJson(*s.currentBasket) : nlohmann::json(nullptr);
    return j;
}

PersistedState stateFromJson(const nlohmann::json& j) {
    const int version = j.at("version").get<int>();
    if (version > PersistedState::kVersion) {
        throw std::runtime_error("state file was written by a newer Muuk (version " + std::to_string(version) + ")");
    }
    PersistedState s;
    s.account = j.value("account", std::string());
    s.symbol = j.value("symbol", std::string());
    s.savedAt = j.value("savedAt", std::string());
    s.engine = engineFromJson(j.at("engine"));
    s.openLegs = legsFromJson(j, "openLegs");
    s.pendingOrders = legsFromJson(j, "pendingOrders");
    s.cancellingOrders = legsFromJson(j, "cancellingOrders");
    s.pendingCloses = legsFromJson(j, "pendingCloses");
    s.waitingOrders = legsFromJson(j, "waitingOrders");
    s.closeFailed = j.value("closeFailed", false);
    if (j.contains("currentBasket") && !j.at("currentBasket").is_null()) {
        s.currentBasket = basketFromJson(j.at("currentBasket"));
    }
    s.closeReason = j.value("closeReason", std::string());
    s.basketCounter = j.value("basketCounter", 0);
    s.nextCancelReqId = j.value("nextCancelReqId", uint64_t{1});
    s.nextCloseReqId = j.value("nextCloseReqId", uint64_t{1});
    s.nextAdoptedId = j.value("nextAdoptedId", uint64_t{1'000'000'000});
    s.statsSince = j.value("statsSince", std::string());
    s.dayKey = j.value("dayKey", std::string());
    s.dayRealized = j.value("dayRealized", 0.0);
    s.dailyLimitHit = j.value("dailyLimitHit", false);
    s.locked = j.value("locked", false);
    s.lockedLoss = j.value("lockedLoss", 0.0);
    s.lockedAt = j.value("lockedAt", std::string());
    s.ticksProcessed = j.value("ticksProcessed", uint64_t{0});
    s.tickDecisionUs = totalsFromJson(j, "tickDecisionUs");
    s.fillReactionUs = totalsFromJson(j, "fillReactionUs");
    s.brokerRoundTripMs = totalsFromJson(j, "brokerRoundTripMs");
    return s;
}

void saveState(const std::filesystem::path& file, const PersistedState& state) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << stateToJson(state).dump(1);
        out.flush();
        if (!out) throw std::runtime_error("write failed: " + tmp.string());
    }
    std::filesystem::rename(tmp, file);  // replaces the old file in one step
}

std::optional<PersistedState> loadState(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::stringstream buf;
    buf << in.rdbuf();
    return stateFromJson(nlohmann::json::parse(buf.str()));
}

void resetStateStatistics(const std::filesystem::path& file) {
    std::optional<PersistedState> s = loadState(file);
    if (!s) return;
    s->ticksProcessed = 0;
    s->tickDecisionUs = {};
    s->fillReactionUs = {};
    s->brokerRoundTripMs = {};
    s->basketCounter = 0;
    s->statsSince = localTimestamp();
    saveState(file, *s);
}

std::filesystem::path dataDirectoryFor(const std::filesystem::path& configPath) {
    std::error_code ec;
    if (!configPath.empty() && configPath.has_parent_path()) {
        const auto abs = std::filesystem::absolute(configPath.parent_path(), ec);
        if (!ec) return abs;
    }
    return std::filesystem::current_path(ec);
}

namespace {
std::string configStem(const std::filesystem::path& configPath) {
    std::string name = configPath.filename().string();
    if (name.size() > 5 && name.substr(name.size() - 5) == ".json") name.resize(name.size() - 5);
    return name.empty() ? std::string("muuk") : name;
}
}  // namespace

std::filesystem::path historyFileFor(const std::filesystem::path& configPath) {
    const std::filesystem::path dir = dataDirectoryFor(configPath) / "muuk_history";
    const std::filesystem::path file = dir / (configStem(configPath) + ".baskets.jsonl");
    std::error_code ec;
    const std::filesystem::path legacy = dir / "baskets.jsonl";
    if (!std::filesystem::exists(file, ec) && std::filesystem::exists(legacy, ec)) {
        std::filesystem::rename(legacy, file, ec);  // the shared file of older versions becomes this account's
    }
    return file;
}

std::filesystem::path defaultStateFile(const std::filesystem::path& configPath) {
    std::string name = configPath.filename().string();
    if (name.size() > 5 && name.substr(name.size() - 5) == ".json") name.resize(name.size() - 5);
    if (name.empty()) name = "muuk";
    return dataDirectoryFor(configPath) / "muuk_state" / (name + ".state.json");
}

}  // namespace muuk::ctrader_fix
