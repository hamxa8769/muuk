#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "muuk/core/strategy_engine.hpp"
#include "muuk/ctrader_fix/basket_history.hpp"
#include "muuk/ctrader_fix/latency_stats.hpp"

namespace muuk::ctrader_fix {

// One position or order as LiveRunner tracks it, in exact ticks/steps.
struct PersistedLeg {
    uint64_t id = 0;
    bool buy = true;
    int64_t volumeSteps = 0;
    int64_t priceTicks = 0;           // fill price (positions) or Stop price (orders)
    int64_t requestedPriceTicks = 0;  // Stop price it was placed at
    std::string positionId;           // broker position id (positions only)
    bool adopted = false;
    std::string closeClOrdId;         // closing order in flight (pendingCloses only)
};

// Everything LiveRunner needs to pick a basket back up in a new process --
// written to disk on every change, read on connect. Holds no credentials.
struct PersistedState {
    static constexpr int kVersion = 1;

    std::string account;  // "<environment>.<traderLogin>" -- a state from another account is ignored
    std::string symbol;
    std::string savedAt;

    // The cycle in progress.
    muuk::core::EngineState engine;
    std::vector<PersistedLeg> openLegs;
    std::vector<PersistedLeg> pendingOrders;    // placed, not filled yet
    std::vector<PersistedLeg> cancellingOrders; // cancel sent, not confirmed yet
    std::vector<PersistedLeg> pendingCloses;    // closing order sent, fill not confirmed yet
    std::vector<PersistedLeg> waitingOrders;    // held back: the market was already past their price
    bool closeFailed = false;
    std::optional<BasketRecord> currentBasket;
    std::string closeReason;
    int basketCounter = 0;

    // Id counters, so no ClOrdID is ever reused across runs.
    uint64_t nextCancelReqId = 1;
    uint64_t nextCloseReqId = 1;
    uint64_t nextAdoptedId = 1'000'000'000;

    // Lifetime statistics (since the last reset).
    std::string statsSince;
    uint64_t ticksProcessed = 0;
    LatencyTracker::Totals tickDecisionUs;
    LatencyTracker::Totals fillReactionUs;
    LatencyTracker::Totals brokerRoundTripMs;
};

[[nodiscard]] nlohmann::json stateToJson(const PersistedState& state);
// Throws on a malformed or newer-version document.
[[nodiscard]] PersistedState stateFromJson(const nlohmann::json& j);

// Writes atomically (temp file + rename), so a crash mid-write never leaves
// a torn file behind. Throws on failure.
void saveState(const std::filesystem::path& file, const PersistedState& state);
// nullopt if the file doesn't exist; throws if it exists but can't be read.
[[nodiscard]] std::optional<PersistedState> loadState(const std::filesystem::path& file);

// Starts the lifetime statistics in a saved state over (speed totals, quote
// count, basket numbering), leaving the saved basket itself alone. For when
// the bot isn't running; a running LiveRunner has resetStats(). No-op if
// there's no state file yet.
void resetStateStatistics(const std::filesystem::path& file);

// Where everything is kept for a given config file: the folder the config
// is in, and in it muuk_state/<config name>.state.json.
[[nodiscard]] std::filesystem::path dataDirectoryFor(const std::filesystem::path& configPath);
[[nodiscard]] std::filesystem::path defaultStateFile(const std::filesystem::path& configPath);

}  // namespace muuk::ctrader_fix
