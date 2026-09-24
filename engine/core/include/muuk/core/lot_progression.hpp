#pragma once

#include "muuk/core/types.hpp"

namespace muuk::core {

// Progressive lot sequence, n is 1-based:
//   multiplier <= 1 (default): L(n) = L0 + (n-1) * deltaL      (0.01, 0.02, 0.03...)  -- proposal §3
//   multiplier  > 1:           L(n) = L0 * multiplier^(n-1)    (x2: 0.01, 0.02, 0.04, 0.08...)
// A multiplied lot is rounded to the nearest volume step.
struct LotProgressionConfig {
    Volume initialLot;   // L0
    Volume lotIncrement; // deltaL (add mode only)
    double multiplier = 1.0;
};

// Computes L(n) and clamps/rounds it against the symbol's volume
// constraints (min/max/step). `sequenceIndex` is 1-based.
[[nodiscard]] Volume computeSequenceLot(const LotProgressionConfig& config,
                                         int sequenceIndex,
                                         const SymbolSpec& spec);

}  // namespace muuk::core
