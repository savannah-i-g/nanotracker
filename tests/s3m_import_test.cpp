// S3M importer verification against the authored fixture
// tests/golden/nttest.s3m (4 PCM channels, one looping 32-byte square
// at c2spd 8363, C-5/C-4 notes plus effects D and I — Tremor is the
// counted-never-silent pin for unsupported effects).
#include "io/import/s3m_importer.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <vector>

TEST_CASE("S3M fixture imports faithfully", "[import][s3m]") {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/nttest.s3m", std::ios::binary);
    REQUIRE(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    nt::io::import::ImportResults results;
    const auto project = nt::io::import::import_s3m(bytes.data(), bytes.size(), results);
    REQUIRE(project.has_value());
    CHECK(results.errors.empty());

    CHECK(project->name == "NTTEST");
    CHECK(project->channels == 4);
    CHECK(project->bpm == 140);
    CHECK(project->speed == 3);
    REQUIRE(project->patterns.size() == 1);
    REQUIRE(project->order_list == std::vector<int>{0});

    const auto& rows = project->patterns[0].rows;
    REQUIRE(rows.size() == 64);
    // Note byte 0x50 = octave 5, semitone 0 → tracker note 61 (C-5).
    CHECK(rows[0][0].note == 61);
    CHECK(rows[0][0].instrument == 1);
    CHECK(rows[0][0].volume == 32);
    // D12 volume slide → engine Axy.
    CHECK(rows[0][1].effect == 0xA);
    CHECK(rows[0][1].effect_param == 0x12);
    // I34 tremor has no engine equivalent: cell stays empty, the drop
    // is tallied in warnings instead of vanishing.
    CHECK(rows[8][2].effect == 0);
    CHECK(rows[8][2].effect_param == 0);
    CHECK(rows[32][3].note == 49); // C-4
    CHECK(rows[32][3].instrument == 1);
    REQUIRE(results.warnings.size() == 1);
    CHECK(results.warnings[0].find("no equivalent") != std::string::npos);

    // Sample stays at source rate (no resample on the S3M path).
    REQUIRE(project->samples.size() == 1);
    const auto& sample = project->samples[0];
    CHECK(sample.id == 1);
    CHECK(sample.name == "SQUARE");
    CHECK(sample.frames == 32);
    CHECK(sample.sample_rate == 8363);
    CHECK(sample.loop_start == 0);
    CHECK(sample.loop_length == 32);
    CHECK(sample.base_note == 72); // MIDI C-5 at c2spd
    CHECK(sample.volume == 64);
    CHECK(sample.pan == 128);
    // The embedded bytes are a well-formed WAV (RIFF magic).
    REQUIRE(sample.original_data.size() > 44);
    CHECK(sample.original_data[0] == 'R');
    CHECK(sample.original_data[1] == 'I');
}

TEST_CASE("S3M importer rejects non-S3M data", "[import][s3m]") {
    const std::vector<std::uint8_t> junk(200, 0x55);
    nt::io::import::ImportResults results;
    CHECK_FALSE(nt::io::import::import_s3m(junk.data(), junk.size(), results).has_value());
    CHECK_FALSE(results.errors.empty());
}
