#include "muuk/core/lot_progression.hpp"

#include <cassert>

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
    const Volume raw{config.initialLot.steps + n * config.lotIncrement.steps};

    const Volume minVol = Volume::fromDouble(spec.volumeMin, spec);
    const Volume maxVol = Volume::fromDouble(spec.volumeMax, spec);
    return raw.clampedTo(minVol, maxVol);
}

}  // namespace muuk::core
