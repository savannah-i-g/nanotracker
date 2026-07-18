// ProjectSession slot management and instrument-table editing. The
// audio engine is constructed but never started: commands queue
// harmlessly and decode falls back to 48 kHz, keeping the test
// device-independent.
#include "app/project_session.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

// Minimal 16-bit mono WAV with a 440 Hz sine.
std::filesystem::path write_test_wav() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "nt_session_test.wav";
    constexpr std::uint32_t kRate = 22050;
    constexpr std::uint32_t kFrames = 2205;
    std::vector<std::uint8_t> bytes;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back((v >> (8 * i)) & 0xFFU);
        }
    };
    const auto u16 = [&](std::uint16_t v) {
        bytes.push_back(v & 0xFFU);
        bytes.push_back((v >> 8U) & 0xFFU);
    };
    const auto tag = [&](const char* t) { bytes.insert(bytes.end(), t, t + 4); };
    tag("RIFF");
    u32(36 + (kFrames * 2));
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    tag("data");
    u32(kFrames * 2);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const auto s =
            static_cast<std::int16_t>(20000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / kRate));
        u16(static_cast<std::uint16_t>(s));
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(
                   bytes.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace

TEST_CASE("session sample slots and instrument table", "[session]") {
    nt::audio::AudioEngine audio; // not started — device-independent
    nt::app::ProjectSession session(audio);
    const auto wav_path = write_test_wav();

    SECTION("slot load decodes, resamples, and registers metadata") {
        REQUIRE(session.load_sample_into_slot(3, wav_path));
        const auto* buffer = session.sample_buffer(3);
        REQUIRE(buffer != nullptr);
        CHECK(buffer->source_rate == 22050);
        CHECK(buffer->rate == 48000);
        CHECK(buffer->frames > 4000); // 0.1s resampled up

        const auto& samples = session.project().samples;
        REQUIRE(samples.size() == 1);
        CHECK(samples[0].id == 3);
        CHECK(samples[0].format == "wav");
        CHECK(samples[0].sample_rate == 22050);
    }

    SECTION("meta edits are undoable; payload fields are protected") {
        REQUIRE(session.load_sample_into_slot(1, wav_path));
        auto meta = session.project().samples[0];
        meta.volume = 20;
        meta.frames = 1; // must be ignored (payload-owned)
        session.set_sample_meta(1, meta);
        CHECK(session.project().samples[0].volume == 20);
        CHECK(session.project().samples[0].frames > 1);
        REQUIRE(session.undo().undo());
        CHECK(session.project().samples[0].volume == 64);
    }

    SECTION("instrument entries grow the table and undo restores it") {
        nt::engine::InstrumentTableEntry entry;
        entry.sample_id = 2;
        entry.bound_tracks = {0, 2};
        session.set_instrument_entry(5, entry);

        const auto& table = session.project().instrument_table;
        REQUIRE(table.size() == 5);
        CHECK(table[4].sample_id == 2);
        CHECK(table[4].bound_tracks == std::vector<int>{0, 2});
        CHECK(table[0].sample_id == 1); // defaults map their own slot

        REQUIRE(session.undo().undo());
        CHECK(session.project().instrument_table.empty());
    }

    SECTION("invalid loads fail cleanly") {
        CHECK_FALSE(session.load_sample_into_slot(0, wav_path));
        CHECK_FALSE(session.load_sample_into_slot(1, "/nonexistent/file.wav"));
    }

    std::filesystem::remove(wav_path);
}
