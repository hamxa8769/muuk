#include "muuk/core/lot_progression.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace muuk::core {

Volume computeSequenceLot(const LotProgressionConfig& config,
                           int sequenceIndex,
                           const SymbolSpec& spec) {
    assert(sequenceIndex >= 1);

    // Volume::steps already counts whole multiples of spec.volumeStep (that
    // is how fromDouble/toDouble define the unit), so as long as
    // initialLot/lotIncrement were built against this same spec, any linear
    // combination of them is automatically volume-step-aligned — no
    // separate rounding step is needed here. Only min/max clamping remains.
    const int64_t n = sequenceIndex - 1;
    const Volume minVol = Volume::fromDouble(spec.volumeMin, spec);
    const Volume maxVol = Volume::fromDouble(spec.volumeMax, spec);

    if (config.multiplier > 1.0) {
        // Geometric: grows fast, so cap before converting (no overflow).
        const double steps =
            static_cast<double>(config.initialLot.steps) * std::pow(config.multiplier, static_cast<double>(n));
        const double capped = std::min(steps, static_cast<double>(maxVol.steps));
        return Volume{static_cast<int64_t>(std::llround(capped))}.clampedTo(minVol, maxVol);
    }

    const Volume raw{config.initialLot.steps + n * config.lotIncrement.steps};
    return raw.clampedTo(minVol, maxVol);
}

}  // namespace muuk::core
