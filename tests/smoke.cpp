// Test-harness smoke test: proves Catch2 wiring and pins the version
// constants to their string form.
#include "app/version.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("version constants agree with the version string", "[smoke]") {
    const std::string expected = std::to_string(nt::kVersionMajor) + "." +
                                 std::to_string(nt::kVersionMinor) + "." +
                                 std::to_string(nt::kVersionPatch);
    REQUIRE(expected == nt::kVersionString);
}
