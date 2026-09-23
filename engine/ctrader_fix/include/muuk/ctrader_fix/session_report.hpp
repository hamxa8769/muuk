#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "muuk/ctrader_fix/basket_history.hpp"
#include "muuk/ctrader_fix/latency_stats.hpp"
#include "muuk/ctrader_fix/live_config.hpp"

namespace muuk::ctrader_fix {

struct SessionReportInput {
    std::string generatedAt;
    std::string sessionStartedAt;  // empty if the bot isn't running
    std::string account;           // e.g. "demo 1234567"
    std::string symbol = "XAUUSD";
    LiveSettings settings;

    LatencySummary tickDecisionUs;
    LatencySummary fillReactionUs;
    LatencySummary brokerRoundTripMs;
    uint64_t ticksProcessed = 0;

    std::vector<BasketRecord> sessionBaskets;  // closed during the current run
    std::vector<BasketRecord> allBaskets;      // the whole saved history (includes the current run)

    int openPositions = 0;
    double openNet = 0.0;  // live net P/L of whatever is still open
};

// Builds the full PDF report: overview (all-time and this session),
// speed, settings, basket P/L charts, a table of every basket, and each
// basket's position tree. Returns the PDF bytes.
[[nodiscard]] std::string buildSessionReportPdf(const SessionReportInput& input);

}  // namespace muuk::ctrader_fix
