// IT importer verification against the authored fixture
// tests/golden/nttest.it (sample mode, 4 channels, one looping signed
// 8-bit square at C5Speed 8363, effects D and M plus a note-off —
// channel volume M is the approximated-but-counted pin).
#include "io/import/it_importer.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <vector>

TEST_CASE("IT fixture imports faithfully", "[import][it]") {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/nttest.it", std::ios::binary);
    REQUIRE(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_it(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    CHECK(project->name == "NTTEST");
    CHECK(project->channels == 4);
    CHECK(project->bpm == 150);
    CHECK(project->speed == 4);
    REQUIRE(project->patterns.size() == 1);
    REQUIRE(project->order_list == std::vector<int>{0});

    const auto& rows = project->patterns[0].rows;
    REQUIRE(rows.size() == 64);
    // IT notes are already 1-based tracker notes: byte 61 = C-5.
    CHECK(rows[0][0].note == 61);
    CHECK(rows[0][0].instrument == 1);
    CHECK(rows[0][0].volume == 32);
    // D12 volume slide → engine Axy.
    CHECK(rows[0][1].effect == 0xA);
    CHECK(rows[0][1].effect_param == 0x12);
    // M30 channel volume lands as set-volume Cxx, tallied as an
    // approximation in warnings rather than converted silently.
    CHECK(rows[8][2].effect == 0xC);
    CHECK(rows[8][2].effect_param == 0x30);
    CHECK(rows[16][0].note == nt::engine::kNoteOff); // IT note byte 255
    CHECK(rows[32][3].note == 49);                   // C-4
    CHECK(rows[32][3].instrument == 1);
    REQUIRE(results.warnings.size() == 1);
    CHECK(results.warnings[0].find("approximated") != std::string::npos);

    // Sample stays at source rate (no resample on the IT path).
    REQUIRE(project->samples.size() == 1);
    const auto& sample = project->samples[0];
    CHECK(sample.id == 1);
    CHECK(sample.name == "SQUARE");
    CHECK(sample.frames == 32);
    CHECK(sample.sample_rate == 8363);
    CHECK(sample.loop_start == 0);
    CHECK(sample.loop_length == 32);
    CHECK(sample.base_note == 72); // MIDI C-5 at C5Speed
    CHECK(sample.volume == 48);
    CHECK(sample.pan == 128);
    // The embedded bytes are a well-formed WAV (RIFF magic).
    REQUIRE(sample.original_data.size() > 44);
    CHECK(sample.original_data[0] == 'R');
    CHECK(sample.original_data[1] == 'I');
}

TEST_CASE("IT importer rejects non-IT data", "[import][it]") {
    const std::vector<std::uint8_t> junk(200, 0x55);
    nt::io::import::ImportResults results;
    CHECK_FALSE(nt::io::import::import_it(junk.data(), junk.size(), results).has_value());
    CHECK_FALSE(results.errors.empty());
}
