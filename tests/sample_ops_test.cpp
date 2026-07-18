// Destructive waveform ops on ProjectSession: deterministic per-op
// checks, the persist-then-decode round-trip, the bounded sample-op
// undo, and generation-fenced reclamation under a live engine. The
// device-independent cases construct the engine without start()
// (commands queue harmlessly, decode falls back to 48 kHz); the live
// reclamation case WARN-skips headless like the other device tests.
//
// Tolerance note: every op re-quantises through PCM16 (resident audio
// is rebuilt from the persisted bytes), so values may move by ~1 LSB
// per op even outside the edited range — comparisons allow 2.5/32768.
#include "app/project_session.h"
#include "audio/sample_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {

constexpr float kLsbTol = 2.5F / 32768.0F;

bool near_value(float a, float b, float tol = kLsbTol) {
    return std::abs(a - b) <= tol;
}

// Mono 48 kHz PCM16 WAV from explicit samples: decoding at the 48 kHz
// fallback rate skips the resampler, so the resident buffer is exactly
// these values (stereo-duplicated) on the int16 grid.
std::filesystem::path write_wav48(const char* name, const std::vector<std::int16_t>& samples) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
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
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    tag("RIFF");
    u32(36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(48000);
    u32(48000 * 2);
    u16(2);
    u16(16);
    tag("data");
    u32(data_bytes);
    for (const std::int16_t s : samples) {
        u16(static_cast<std::uint16_t>(s));
    }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                   bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

// Deterministic non-symmetric waveform, peak 15000.
std::vector<std::int16_t> ramp_wave(std::uint32_t frames) {
    std::vector<std::int16_t> samples(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<std::int16_t>(
            15000.0 * std::sin(2.0 * 3.14159265 * 7.0 * i / frames) * (0.25 + (0.75 * i / frames)));
    }
    return samples;
}

std::vector<float> resident_copy(const nt::app::ProjectSession& session, int slot) {
    const nt::audio::SampleBuffer* buffer = session.sample_buffer(slot);
    REQUIRE(buffer != nullptr);
    return buffer->interleaved;
}

const nt::engine::TrackerSample& sample_meta(const nt::app::ProjectSession& session, int slot) {
    for (const nt::engine::TrackerSample& s : session.project().samples) {
        if (s.id == slot) {
            return s;
        }
    }
    FAIL("sample meta missing for slot " << slot);
    static nt::engine::TrackerSample unreachable;
    return unreachable;
}

} // namespace

TEST_CASE("trim keeps the range and shifts loop points", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_trim.wav", ramp_wave(4000))));

    nt::engine::TrackerSample meta = sample_meta(session, 1);
    meta.loop_start = 1000;
    meta.loop_length = 800;
    session.set_sample_meta(1, meta);

    const std::vector<float> before = resident_copy(session, 1);
    REQUIRE(session.sample_trim(1, 500, 3000));

    const nt::audio::SampleBuffer* buffer = session.sample_buffer(1);
    REQUIRE(buffer != nullptr);
    CHECK(buffer->frames == 2500);
    CHECK(buffer->rate == 48000);
    CHECK(buffer->source_rate == 48000);
    const nt::engine::TrackerSample& trimmed = sample_meta(session, 1);
    CHECK(trimmed.frames == 2500);
    CHECK(trimmed.sample_rate == 48000);
    CHECK(trimmed.format == "wav");
    CHECK(trimmed.loop_start == 500);  // 1000 - 500
    CHECK(trimmed.loop_length == 800); // fully inside the kept range
    for (std::uint32_t i = 0; i < 2500; i += 313) {
        CHECK(near_value(buffer->interleaved[static_cast<std::size_t>(i) * 2],
                         before[(static_cast<std::size_t>(i) + 500) * 2]));
    }

    // Cut through the loop tail: [500,1300) intersected with a 600-
    // frame keep clamps to [500,600).
    REQUIRE(session.sample_trim(1, 0, 600));
    CHECK(sample_meta(session, 1).loop_start == 500);
    CHECK(sample_meta(session, 1).loop_length == 100);

    // Loop entirely outside the keep → cleared.
    REQUIRE(session.sample_trim(1, 0, 400));
    CHECK(sample_meta(session, 1).loop_start == 0);
    CHECK(sample_meta(session, 1).loop_length == 0);

    // Degenerate range refuses.
    CHECK(!session.sample_trim(1, 200, 200));
}

TEST_CASE("silence zeroes exactly the range", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_silence.wav", ramp_wave(1000))));
    const std::vector<float> before = resident_copy(session, 1);

    REQUIRE(session.sample_silence(1, 100, 200));
    const std::vector<float> after = resident_copy(session, 1);
    REQUIRE(after.size() == before.size());
    for (std::uint32_t i = 0; i < 1000; ++i) {
        const std::size_t at = static_cast<std::size_t>(i) * 2;
        if (i >= 100 && i < 200) {
            CHECK(after[at] == 0.0F); // silence survives PCM16 exactly
            CHECK(after[at + 1] == 0.0F);
        } else {
            CHECK(near_value(after[at], before[at]));
        }
    }
}

TEST_CASE("fades apply the web envelope grid in both shapes", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_fade.wav", ramp_wave(1000))));
    const std::vector<float> before = resident_copy(session, 1);

    SECTION("linear fade in") {
        REQUIRE(session.sample_fade_in(1, 0, 1000, nt::io::FadeShape::kLinear));
        const std::vector<float> after = resident_copy(session, 1);
        CHECK(after[0] == 0.0F); // t=0 multiplies by exactly 0
        for (std::uint32_t i = 100; i < 1000; i += 250) {
            const float expected = before[static_cast<std::size_t>(i) * 2] * (i / 1000.0F);
            CHECK(near_value(after[static_cast<std::size_t>(i) * 2], expected));
        }
    }
    SECTION("equal-power fade in lands sin(t*pi/2)") {
        REQUIRE(session.sample_fade_in(1, 0, 1000, nt::io::FadeShape::kEqualPower));
        const std::vector<float> after = resident_copy(session, 1);
        const float expected =
            before[500 * 2] * static_cast<float>(std::sin(0.5 * 3.14159265 / 2.0));
        CHECK(near_value(after[500 * 2], expected));
    }
    SECTION("linear fade out mirrors the grid") {
        REQUIRE(session.sample_fade_out(1, 0, 1000, nt::io::FadeShape::kLinear));
        const std::vector<float> after = resident_copy(session, 1);
        CHECK(near_value(after[0], before[0])); // full level at range start
        for (std::uint32_t i = 250; i < 1000; i += 250) {
            const float expected = before[static_cast<std::size_t>(i) * 2] * (1.0F - (i / 1000.0F));
            CHECK(near_value(after[static_cast<std::size_t>(i) * 2], expected));
        }
    }
}

TEST_CASE("normalize hits the peak target scaling up and down", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_norm.wav", ramp_wave(1000))));

    const auto peak_of = [&session] {
        float peak = 0.0F;
        for (const float v : resident_copy(session, 1)) {
            peak = std::max(peak, std::abs(v));
        }
        return peak;
    };

    REQUIRE(session.sample_normalize(1, 0, UINT32_MAX, 1.0F));
    CHECK(near_value(peak_of(), 1.0F, 2.0F * kLsbTol));

    // Downward normalize from full scale — the web editor refused any
    // peak >= 0.999 (WaveformEditor.tsx:278); the native op scales down.
    REQUIRE(session.sample_normalize(1, 0, UINT32_MAX, 0.5F));
    CHECK(near_value(peak_of(), 0.5F, 2.0F * kLsbTol));

    // Silent selection refuses.
    REQUIRE(session.sample_silence(1, 0, 64));
    CHECK(!session.sample_normalize(1, 0, 64, 1.0F));
}

TEST_CASE("reverse is frame-symmetric within the range and keeps the loop", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_rev.wav", ramp_wave(1000))));
    nt::engine::TrackerSample meta = sample_meta(session, 1);
    meta.loop_start = 300;
    meta.loop_length = 400;
    session.set_sample_meta(1, meta);
    const std::vector<float> before = resident_copy(session, 1);

    REQUIRE(session.sample_reverse(1, 100, 900));
    const std::vector<float> after = resident_copy(session, 1);
    for (std::uint32_t i = 0; i < 800; i += 97) {
        const std::size_t at = static_cast<std::size_t>(100 + i) * 2;
        const std::size_t mirrored = static_cast<std::size_t>(899 - i) * 2;
        CHECK(near_value(after[at], before[mirrored]));
        CHECK(near_value(after[at + 1], before[mirrored + 1]));
    }
    CHECK(near_value(after[50 * 2], before[50 * 2])); // outside untouched
    CHECK(near_value(after[950 * 2], before[950 * 2]));
    // Loop window preserved (not mirrored) — documented op semantics.
    CHECK(sample_meta(session, 1).loop_start == 300);
    CHECK(sample_meta(session, 1).loop_length == 400);
}

TEST_CASE("gain applies the dB factor", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_gain.wav", ramp_wave(1000))));
    const std::vector<float> before = resident_copy(session, 1);

    REQUIRE(session.sample_gain_db(1, 0, UINT32_MAX, -6.0206F)); // ×0.5
    const std::vector<float> after = resident_copy(session, 1);
    for (std::uint32_t i = 0; i < 1000; i += 111) {
        const std::size_t at = static_cast<std::size_t>(i) * 2;
        CHECK(near_value(after[at], before[at] * 0.5F));
    }
}

TEST_CASE("DC removal centres both channels", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    std::vector<std::int16_t> offset_wave = ramp_wave(1000);
    for (std::int16_t& s : offset_wave) {
        s = static_cast<std::int16_t>((s / 2) + 8000); // strong DC offset
    }
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_dc.wav", offset_wave)));

    REQUIRE(session.sample_remove_dc(1));
    const std::vector<float> after = resident_copy(session, 1);
    double mean_l = 0.0;
    double mean_r = 0.0;
    for (std::size_t i = 0; i < after.size(); i += 2) {
        mean_l += after[i];
        mean_r += after[i + 1];
    }
    mean_l /= static_cast<double>(after.size() / 2);
    mean_r /= static_cast<double>(after.size() / 2);
    CHECK(std::abs(mean_l) < 5e-4);
    CHECK(std::abs(mean_r) < 5e-4);
}

TEST_CASE("original_data re-decodes to exactly the resident audio", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_rt.wav", ramp_wave(1000))));
    REQUIRE(session.sample_gain_db(1, 0, UINT32_MAX, 3.0F));

    const nt::engine::TrackerSample& meta = sample_meta(session, 1);
    CHECK(meta.format == "wav");
    CHECK(meta.sample_rate == 48000);
    CHECK(meta.num_channels == 2);
    std::string format;
    std::string error;
    const auto decoded = nt::audio::load_sample_memory(
        meta.original_data.data(), meta.original_data.size(), 48000, format, error);
    REQUIRE(decoded != nullptr);
    CHECK(format == "wav");
    const nt::audio::SampleBuffer* resident = session.sample_buffer(1);
    REQUIRE(resident != nullptr);
    REQUIRE(decoded->frames == resident->frames);
    CHECK(decoded->interleaved == resident->interleaved); // bit-exact
}

TEST_CASE("sample-op undo is bounded, exact and evicts oldest-first", "[sample_ops]") {
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_undo.wav", ramp_wave(1000))));

    SECTION("round trip restores the exact buffer object and metadata") {
        nt::engine::TrackerSample meta = sample_meta(session, 1);
        meta.loop_start = 200;
        meta.loop_length = 300;
        session.set_sample_meta(1, meta);
        const std::vector<float> before = resident_copy(session, 1);

        REQUIRE(session.sample_trim(1, 100, 900));
        const std::vector<float> edited = resident_copy(session, 1);
        REQUIRE(session.undo().undo());
        CHECK(resident_copy(session, 1) == before); // same object, bit-exact
        CHECK(sample_meta(session, 1).loop_start == 200);
        CHECK(sample_meta(session, 1).loop_length == 300);
        CHECK(sample_meta(session, 1).frames == 1000);
        REQUIRE(session.undo().redo());
        CHECK(resident_copy(session, 1) == edited);
        CHECK(sample_meta(session, 1).frames == 800);
    }

    SECTION("depth caps at kMaxSampleOps, dropping older history with the evictee") {
        // One cheap entry older than every sample op…
        session.set_cell(0, 0, 0, nt::engine::TrackerCell{});
        CHECK(session.undo().can_undo());
        // …then one op past the cap.
        for (int i = 0; i < static_cast<int>(nt::app::UndoStack::kMaxSampleOps) + 1; ++i) {
            REQUIRE(session.sample_gain_db(1, 0, UINT32_MAX, (i % 2) != 0 ? 0.2F : -0.2F));
        }
        // Exactly kMaxSampleOps entries survive; the evicted oldest op
        // took the cell edit (older than it) along, keeping history a
        // contiguous suffix.
        for (std::size_t i = 0; i < nt::app::UndoStack::kMaxSampleOps; ++i) {
            CHECK(session.undo().next_undo_label() == std::string("gain"));
            REQUIRE(session.undo().undo());
        }
        CHECK(!session.undo().can_undo());
    }
}

TEST_CASE("retired buffers free behind the fence under a live engine", "[sample_ops][reclaim]") {
    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_sample_into_slot(1, write_wav48("nt_ops_stress.wav", ramp_wave(24000))));

    // Cycle audition + destructive replacement: every op retires the
    // previous buffer while a voice may still be sounding it; the
    // fence must both keep it alive through the swap and free it soon
    // after. ASan turns any early free into a hard failure.
    for (int i = 0; i < 40; ++i) {
        session.preview_note(0, 1, 49);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(session.sample_gain_db(1, 0, UINT32_MAX, (i % 2) != 0 ? 0.3F : -0.3F));
        session.sweep_retired();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(session.reclaimer().retired_count() >= 40);

    // Let the engine apply the final publish, then drain completely.
    for (int i = 0; i < 200 && session.reclaimer().live_count() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        session.sweep_retired();
    }
    CHECK(session.reclaimer().freed_count() > 0);
    CHECK(session.reclaimer().live_count() == 0);

    session.stop();
    audio.stop();
}
