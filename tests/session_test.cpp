// ProjectSession slot management and instrument-table editing. The
// audio engine is constructed but never started: commands queue
// harmlessly and decode falls back to 48 kHz, keeping the test
// device-independent.
#include "app/project_session.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

// Minimal 16-bit mono WAV with a 440 Hz sine. `name` keeps concurrent
// ctest processes off each other's temp files.
std::filesystem::path write_test_wav(const char* name = "nt_session_test.wav") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
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

TEST_CASE("session pattern create/delete keeps id==index and remaps the order list",
          "[session][pattern]") {
    nt::audio::AudioEngine audio; // not started — device-independent
    nt::app::ProjectSession session(audio);

    // Fresh project: one pattern (id 0), order [0].
    REQUIRE(session.project().patterns.size() == 1);
    REQUIRE(session.project().order_list == std::vector<int>{0});

    SECTION("create appends a blank pattern with id == index") {
        const int a = session.create_pattern();
        const int b = session.create_pattern();
        CHECK(a == 1);
        CHECK(b == 2);
        const auto& proj = session.project();
        REQUIRE(proj.patterns.size() == 3);
        CHECK(proj.patterns[1].id == 1);
        CHECK(proj.patterns[2].id == 2);
        // Blank at the project default row count and channel width.
        CHECK(static_cast<int>(proj.patterns[2].rows.size()) == proj.rows_per_pattern);
        CHECK(proj.patterns[2].rows[0].size() == static_cast<std::size_t>(proj.channels));
        CHECK(proj.order_list == std::vector<int>{0}); // create never touches order
    }

    SECTION("delete removes refs and shifts higher refs down (index==id preserved)") {
        session.create_pattern(); // 1
        session.create_pattern(); // 2
        session.order_insert(1, 1);
        session.order_insert(2, 2);
        session.order_insert(3, 1);
        REQUIRE(session.project().order_list == std::vector<int>{0, 1, 2, 1});

        REQUIRE(session.delete_pattern(1));
        const auto& proj = session.project();
        REQUIRE(proj.patterns.size() == 2);
        CHECK(proj.patterns[0].id == 0);
        CHECK(proj.patterns[1].id == 1); // old pattern 2 renumbered to 1
        // refs to 1 dropped, refs to 2 decremented to 1.
        CHECK(proj.order_list == std::vector<int>{0, 1});
    }

    SECTION("deleting the last remaining pattern is refused") {
        CHECK_FALSE(session.delete_pattern(0));
        CHECK(session.project().patterns.size() == 1);
    }

    SECTION("delete that empties the order list re-seeds it") {
        session.create_pattern(); // 1
        REQUIRE(session.set_order_list({1, 1}));
        REQUIRE(session.delete_pattern(1)); // every ref pointed at 1
        // patterns=[0]; order must never be empty.
        CHECK(session.project().patterns.size() == 1);
        CHECK_FALSE(session.project().order_list.empty());
        CHECK(session.project().order_list.front() == 0);
    }
}

TEST_CASE("session order-list edits and refusals", "[session][pattern]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    session.create_pattern(); // 1
    session.create_pattern(); // 2

    SECTION("insert / move / repoint") {
        REQUIRE(session.order_insert(1, 1));
        REQUIRE(session.order_insert(2, 2));
        CHECK(session.project().order_list == std::vector<int>{0, 1, 2});
        REQUIRE(session.order_move(0, 1)); // swap 0 and 1
        CHECK(session.project().order_list == std::vector<int>{1, 0, 2});
        REQUIRE(session.order_set(0, 2));
        CHECK(session.project().order_list == std::vector<int>{2, 0, 2});
    }

    SECTION("remove refuses to empty the list") {
        REQUIRE(session.order_insert(1, 1));
        REQUIRE(session.order_remove(1));
        CHECK(session.project().order_list == std::vector<int>{0});
        CHECK_FALSE(session.order_remove(0)); // last entry
        CHECK(session.project().order_list == std::vector<int>{0});
    }

    SECTION("out-of-range and invalid refs are refused") {
        CHECK_FALSE(session.order_insert(0, 99));    // pattern out of range
        CHECK_FALSE(session.order_move(0, -1));      // off the top
        CHECK_FALSE(session.order_set(5, 0));        // position out of range
        CHECK_FALSE(session.set_order_list({}));     // empty
        CHECK_FALSE(session.set_order_list({0, 7})); // ref out of range
        CHECK(session.project().order_list == std::vector<int>{0});
    }

    SECTION("set_order_list replaces the whole list") {
        REQUIRE(session.set_order_list({2, 1, 0, 1}));
        CHECK(session.project().order_list == std::vector<int>{2, 1, 0, 1});
    }
}

TEST_CASE("session resize_pattern pads, truncates and clamps sequence notes",
          "[session][pattern]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    const int speed = session.project().speed;

    SECTION("grow pads empty rows, shrink truncates, bounds clamp to 1..256") {
        REQUIRE(session.resize_pattern(0, 32));
        CHECK(session.project().patterns[0].rows.size() == 32);
        REQUIRE(session.resize_pattern(0, 128));
        CHECK(session.project().patterns[0].rows.size() == 128);
        REQUIRE(session.resize_pattern(0, 0)); // clamps to 1
        CHECK(session.project().patterns[0].rows.size() == 1);
        REQUIRE(session.resize_pattern(0, 9999)); // clamps to 256
        CHECK(session.project().patterns[0].rows.size() == 256);
    }

    SECTION("shrink drops notes past the end and trims overhang") {
        REQUIRE(session.resize_pattern(0, 64));
        session.seq_add_note(
            0, 0, 0,
            {.pitch = 60, .start_tick = 8 * speed, .duration_ticks = 40 * speed, .velocity = 100});
        session.seq_add_note(
            0, 0, 0,
            {.pitch = 62, .start_tick = 40 * speed, .duration_ticks = speed, .velocity = 100});
        REQUIRE(session.resize_pattern(0, 16)); // max_tick = 16*speed
        const auto* layer = session.seq_layer(0, 0, 0, false);
        REQUIRE(layer != nullptr);
        REQUIRE(layer->notes.size() == 1); // the row-40 note is gone
        CHECK(layer->notes[0].pitch == 60);
        // overhang trimmed so start+duration <= 16*speed.
        CHECK(layer->notes[0].start_tick + layer->notes[0].duration_ticks <= 16 * speed);
    }
}

TEST_CASE("pattern/order structural edits stay consistent under a live engine",
          "[session][pattern][reclaim]") {
    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::app::ProjectSession session(audio);
    for (int i = 0; i < 3; ++i) {
        session.create_pattern(); // patterns 0..3
    }
    session.order_insert(1, 1);
    session.order_insert(2, 2);
    session.order_insert(3, 3);
    session.play();

    // Mutate the pattern set and order list while a real audio thread
    // renders them through the bundle. A stale index or a freed pattern
    // vector would crash or trip ASan; each op stops + republishes, so we
    // re-arm the transport to keep it rolling across the edits.
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (i == 5) {
            session.create_pattern();
        } else if (i == 10) {
            session.delete_pattern(1);
        } else if (i == 15) {
            session.order_move(0, 1);
        } else if (i == 20) {
            session.order_remove(0);
        } else if (i == 25) {
            session.resize_pattern(0, 48);
        }
        session.play();
        session.sweep_retired();
    }

    const auto& proj = session.project();
    for (const int pat : proj.order_list) {
        CHECK(pat >= 0);
        CHECK(pat < static_cast<int>(proj.patterns.size()));
    }
    for (int i = 0; i < static_cast<int>(proj.patterns.size()); ++i) {
        CHECK(proj.patterns[static_cast<std::size_t>(i)].id == i);
    }
    CHECK_FALSE(proj.order_list.empty());

    session.stop();
    audio.stop();
}

TEST_CASE("session preview_file auditions through the reclamation fence", "[session][preview]") {
    const std::filesystem::path wav = write_test_wav("nt_session_preview.wav");
    // Offline engine so the fence advances deterministically: each
    // render pull drains the preview command and republishes the serial.
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);
    std::vector<float> block(256, 0.0F);
    const auto pump = [&] {
        for (int i = 0; i < 4; ++i) {
            audio.render_offline(block.data(), 128);
        }
    };
    pump(); // apply the session's construction publish

    // First audition decodes and holds the buffer; nothing to retire.
    REQUIRE(session.preview_file(wav));
    REQUIRE(session.preview_buffer() != nullptr);
    CHECK(session.reclaimer().retired_count() == 0);
    pump();
    session.sweep_retired();
    CHECK(session.reclaimer().freed_count() == 0);

    // Replacing the audition retires the first buffer behind the fence;
    // once the engine proves the serial, the sweep frees exactly it.
    REQUIRE(session.preview_file(wav));
    CHECK(session.reclaimer().retired_count() == 1);
    pump();
    session.sweep_retired();
    CHECK(session.reclaimer().freed_count() == 1);

    // Clearing the preview retires the second buffer the same way — no
    // process-lifetime cache survives.
    session.stop_preview();
    CHECK(session.preview_buffer() == nullptr);
    CHECK(session.reclaimer().retired_count() == 2);
    pump();
    session.sweep_retired();
    CHECK(session.reclaimer().freed_count() == 2);

    audio.stop();
    std::filesystem::remove(wav);
}
