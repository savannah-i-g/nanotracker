// XM importer verification against tests/golden/xmtest.xm (2 channels,
// linear frequency table, one looping delta-encoded square with
// relNote +12 — the base-note sign regression pin, fix #1).
#include "io/import/xm_importer.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <vector>

TEST_CASE("XM fixture imports faithfully", "[import][xm]") {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/xmtest.xm", std::ios::binary);
    REQUIRE(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_xm(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    CHECK(project->name == "XMTEST");
    CHECK(project->channels == 2);
    CHECK(project->freq_table == nt::engine::FreqTable::kLinear);
    CHECK(project->bpm == 125);
    CHECK(project->speed == 6);

    REQUIRE(project->patterns.size() == 1);
    REQUIRE(project->patterns[0].rows.size() == 4);
    const auto& cell = project->patterns[0].rows[0][0];
    CHECK(cell.note == 61); // C-5
    CHECK(cell.instrument == 1);
    CHECK(cell.volume == 0xFF);

    REQUIRE(project->samples.size() == 1);
    const auto& sample = project->samples[0];
    CHECK(sample.frames == 32);
    CHECK(sample.loop_start == 0);
    CHECK(sample.loop_length == 32);
    CHECK(sample.sample_rate == 8363);
    // relNote +12 tunes the sample UP an octave, so the engine's
    // reference note goes DOWN an octave: base_note = 72 - 12. The web
    // app once shipped 72 + relNote; this pin prevents the regression.
    CHECK(sample.base_note == 60);
}

TEST_CASE("XM importer rejects non-XM data", "[import][xm]") {
    const std::vector<std::uint8_t> junk(200, 0x55);
    nt::io::import::ImportResults results;
    CHECK_FALSE(nt::io::import::import_xm(junk.data(), junk.size(), results).has_value());
    CHECK_FALSE(results.errors.empty());
}
