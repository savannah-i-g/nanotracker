// MOD importer verification against the authored fixture
// tests/golden/nttest.mod (one looping 32-byte square, C-7 notes at
// rows 0 and 32 of a 4-channel M.K. module).
#include "io/import/mod_importer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <vector>

TEST_CASE("MOD fixture imports faithfully", "[import][mod]") {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/nttest.mod", std::ios::binary);
    REQUIRE(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_mod(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    CHECK(project->name == "NTTEST");
    CHECK(project->channels == 4);
    CHECK(project->bpm == 125);
    CHECK(project->speed == 6);
    REQUIRE(project->patterns.size() == 1);
    REQUIRE(project->order_list == std::vector<int>{0});

    // Period 214 = one octave above MOD C-2 (428) → tracker note 85.
    const auto& rows = project->patterns[0].rows;
    CHECK(rows[0][0].note == 85);
    CHECK(rows[0][0].instrument == 1);
    CHECK(rows[0][0].volume == 0xFF);
    CHECK(rows[32][0].note == 85);
    CHECK(rows[1][0].note == 0);

    // Sample: 32 bytes upsampled 8287→44100; loop covers the whole cycle.
    REQUIRE(project->samples.size() == 1);
    const auto& sample = project->samples[0];
    CHECK(sample.id == 1);
    CHECK(sample.name == "SQUARE");
    CHECK(sample.base_note == nt::io::import::kModBaseNote);
    CHECK(sample.volume == 64);
    const double ratio = 44100.0 / nt::io::import::kModSampleRate;
    CHECK(sample.frames == static_cast<std::uint32_t>(std::ceil(32 * ratio)));
    CHECK(sample.loop_start == 0);
    CHECK(sample.loop_length == static_cast<std::uint32_t>(std::lround(32 * ratio)));
    // The embedded bytes are a well-formed WAV (RIFF magic).
    REQUIRE(sample.original_data.size() > 44);
    CHECK(sample.original_data[0] == 'R');
    CHECK(sample.original_data[1] == 'I');
}
