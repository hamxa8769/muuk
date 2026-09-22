#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "muuk/core/types.hpp"

using namespace muuk::core;

namespace {
SymbolSpec xauusdSpec() {
    SymbolSpec spec;
    spec.name = "XAUUSD";
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

TEST_CASE("SymbolSpec validity") {
    SymbolSpec spec = xauusdSpec();
    REQUIRE(spec.isValid());

    SymbolSpec bad = spec;
    bad.tickSize = 0.0;
    REQUIRE_FALSE(bad.isValid());

    bad = spec;
    bad.volumeMax = 0.0;
    REQUIRE_FALSE(bad.isValid());
}

TEST_CASE("Price round-trips through double conversion") {
    const SymbolSpec spec = xauusdSpec();

    const Price p = Price::fromDouble(4337.00, spec);
    REQUIRE(p.ticks == 433700);
    REQUIRE(p.toDouble(spec) == Catch::Approx(4337.00));

    const Price q = Price::fromDouble(4336.00, spec);
    REQUIRE((p - q).ticks == 100);
    REQUIRE((q + Price{100}).ticks == p.ticks);
}

TEST_CASE("Price ordering") {
    const SymbolSpec spec = xauusdSpec();
    const Price low = Price::fromDouble(4336.00, spec);
    const Price high = Price::fromDouble(4337.00, spec);
    REQUIRE(low < high);
    REQUIRE(high > low);
    REQUIRE(low == Price::fromDouble(4336.00, spec));
}

TEST_CASE("Volume round-trips and clamps") {
    const SymbolSpec spec = xauusdSpec();

    const Volume v = Volume::fromDouble(0.05, spec);
    REQUIRE(v.steps == 5);
    REQUIRE(v.toDouble(spec) == Catch::Approx(0.05));

    const Volume min = Volume::fromDouble(0.01, spec);
    const Volume max = Volume::fromDouble(1.0, spec);
    REQUIRE(Volume::fromDouble(0.001, spec).clampedTo(min, max).steps == min.steps);
    REQUIRE(Volume::fromDouble(2.0, spec).clampedTo(min, max).steps == max.steps);
    REQUIRE(Volume::fromDouble(0.5, spec).clampedTo(min, max).steps == Volume::fromDouble(0.5, spec).steps);
}
