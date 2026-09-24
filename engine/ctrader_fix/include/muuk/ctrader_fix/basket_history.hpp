#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace muuk::ctrader_fix {

// One position inside a basket, from open to close, in plain human units.
struct BasketLegRecord {
    uint64_t id = 0;              // LiveRunner's handle (engine order id, or an adopted-position handle)
    std::string positionId;       // broker position id (tag 721)
    bool buy = true;
    double lots = 0.0;
    double requestedPrice = 0.0;  // Stop price it was placed at
    double entryPrice = 0.0;      // actual fill
    double exitPrice = 0.0;       // actual closing fill; 0 until closed
    double gross = 0.0;           // (exit - entry) in money, from real fills
    double commission = 0.0;
    double net = 0.0;
    std::string openedAt;
    std::string closedAt;         // empty while still open
    bool adopted = false;         // opened before this session, found by a position sync
    std::string note;             // e.g. "closed while the bot was disconnected"
};

// One basket (range cycle): every position it opened and how it ended.
struct BasketRecord {
    int number = 0;               // 1, 2, 3... within the session that produced it
    std::string session;          // when that session started, to group records across runs
    std::string symbol;
    std::string startedAt;
    std::string closedAt;
    double durationSeconds = 0.0;
    double contractSize = 100.0;  // units per 1.0 lot (XAUUSD: 100 oz) -- for traded notional
    double rangeLower = 0.0;
    double rangeUpper = 0.0;
    std::string closeReason;      // "profit target", "manual: close range", ...
    std::vector<BasketLegRecord> legs;
    double gross = 0.0;
    double commission = 0.0;
    double net = 0.0;
};

[[nodiscard]] nlohmann::json basketToJson(const BasketRecord& basket);
[[nodiscard]] BasketRecord basketFromJson(const nlohmann::json& j);

// History file: one JSON object per line (JSON Lines), appended as each
// basket completes -- a crash can lose at most the basket in progress,
// never the ones already written.
void appendBasketHistory(const std::filesystem::path& file, const BasketRecord& basket);
// Missing file = empty history; unreadable lines are skipped.
[[nodiscard]] std::vector<BasketRecord> loadBasketHistory(const std::filesystem::path& file);

// Local wall-clock time, "YYYY-MM-DD HH:MM:SS".
[[nodiscard]] std::string localTimestamp();

}  // namespace muuk::ctrader_fix
