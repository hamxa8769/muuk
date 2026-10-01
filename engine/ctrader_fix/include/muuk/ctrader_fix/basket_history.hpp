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
    std::string marketSession;    // trading session(s) open when it started: "London + New York", "Tokyo"...
    std::string symbol;
    std::string startedAt;
    std::string closedAt;
    double durationSeconds = 0.0;
    double contractSize = 100.0;  // units per 1.0 lot (XAUUSD: 100 oz) -- for traded notional
    double rangeLower = 0.0;
    double rangeUpper = 0.0;
    std::string closeReason;      // "profit target", "manual: close range", ...
    // How it ended, in one word: "target", "stop loss", "manual", "interrupted",
    // "guard" (closed on app exit), "failed" (a close was rejected), "closed".
    std::string outcome;
    int ordersPlaced = 0;         // pending orders sent to the broker
    int ordersFilled = 0;
    int ordersRejected = 0;
    int ordersMissed = 0;         // found missing at the broker and placed again
    double worstNet = 0.0;        // lowest floating net P/L while it ran (its deepest drawdown)
    double bestNet = 0.0;
    double maxOpenLots = 0.0;     // most lots open at once
    // Net P/L the bot saw from the live quotes when the target / stop loss
    // fired (0 = closed some other way). net - netAtTrigger is what the
    // closing market orders gained or lost to slippage on the way out.
    double netAtTrigger = 0.0;
    // What protected the basket on the way.
    bool guardHeld = false;       // the margin guard held back a level (a lock stop waited at its line)
    bool locked = false;          // a lock froze the loss (lock & pause, or the guard's lock stop filled)
    double lockedLoss = 0.0;      // the net it was frozen at
    int escapeTries = 0;          // escape trades out of the lock
    int breakEvenFromLevel = 0;   // >0: it closed on the break-even target (in force from this level)
    std::vector<BasketLegRecord> legs;
    double gross = 0.0;
    double commission = 0.0;
    double net = 0.0;
};

// How a basket ended, in words a trader reads at a glance -- the same in
// the app's History and in the PDF report.
enum class BasketVerdictKind {
    Profit,     // reached the profit target without ever going against the first position
    Recovered,  // was in loss on the way (or guarded / locked) and still closed at or above 0
    Loss,       // closed in loss
    Neutral,    // closed at about 0 some other way
};
struct BasketVerdict {
    BasketVerdictKind kind = BasketVerdictKind::Neutral;
    std::string label;   // "Profit target", "Loss recovered -> profit", "Stop loss", ...
    std::string detail;  // what acted on the way: "margin guard held a level, locked at -12.40", ...
};
[[nodiscard]] BasketVerdict basketVerdict(const BasketRecord& basket);

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
