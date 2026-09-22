#pragma once

#include "muuk/core/types.hpp"

namespace muuk::core {

// Progressive lot sequence: L(n) = L0 + (n-1) * deltaL, n is 1-based.
// Proposal §3.
struct LotProgressionConfig {
    Volume initialLot;   // L0
    Volume lotIncrement; // deltaL
};

// Computes L(n) and clamps/rounds it against the symbol's volume
// constraints (min/max/step). `sequenceIndex` is 1-based.
[[nodiscard]] Volume computeSequenceLot(const LotProgressionConfig& config,
                                         int sequenceIndex,
                                         const SymbolSpec& spec);

}  // namespace muuk::core
