#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/lot_progression.hpp"

using namespace muuk::core;

namespace {
SymbolSpec xauusdSpec() {
    SymbolSpec spec;
    spec.digits = 2;
    spec.tickSize = 0.01;
    spec.tickValue = 1.0;
    spec.contractSize = 100.0;
    spec.volumeStep = 0.01;
    spec.volumeMin = 0.01;
    spec.volumeMax = 100.0;
    return spec;
}
}  // namespace

TEST_CASE("Lot progression matches the proposal worked example (L0=0.01, deltaL=0.01)") {
    const SymbolSpec spec = xauusdSpec();
    LotProgressionConfig cfg{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};

    const double expected[] = {0.01, 0.02, 0.03, 0.04, 0.05, 0.06};
    for (int n = 1; n <= 6; ++n) {
        const Volume lot = computeSequenceLot(cfg, n, spec);
        REQUIRE(lot.toDouble(spec) == Catch::Approx(expected[n - 1]));
    }
}

TEST_CASE("Lot progression clamps to symbol volume max") {
    SymbolSpec spec = xauusdSpec();
    spec.volumeMax = 0.05;
    LotProgressionConfig cfg{Volume::fromDouble(0.01, spec), Volume::fromDouble(0.01, spec)};

    const Volume lot = computeSequenceLot(cfg, 10, spec); // would be 0.10 unclamped
    REQUIRE(lot.toDouble(spec) == Catch::Approx(0.05));
}

TEST_CASE("Lot progression clamps to symbol volume min") {
    SymbolSpec spec = xauusdSpec();
    spec.volumeMin = 0.02;
    LotProgressionConfig cfg{Volume::fromDouble(0.001, spec), Volume{0}};

    const Volume lot = computeSequenceLot(cfg, 1, spec);
    REQUIRE(lot.toDouble(spec) == Catch::Approx(0.02));
}

TEST_CASE("Lot progression stays aligned to a non-default volume step") {
    SymbolSpec spec = xauusdSpec();
    spec.volumeStep = 0.05;
    spec.volumeMin = 0.05;
    LotProgressionConfig cfg{Volume::fromDouble(0.05, spec), Volume::fromDouble(0.05, spec)};

    for (int n = 1; n <= 4; ++n) {
        const Volume lot = computeSequenceLot(cfg, n, spec);
        const double lots = lot.toDouble(spec);
        REQUIRE(lots == Catch::Approx(0.05 * n));
        // Must be an exact multiple of the broker's volume step.
        REQUIRE(lot.steps % Volume::fromDouble(spec.volumeStep, spec).steps == 0);
    }
}
