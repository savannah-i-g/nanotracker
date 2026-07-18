// NTP v1 verification: manifest validation (strict, collected errors),
// archive loading through a real in-memory ZIP, and deterministic
// instance processing — a playable synth voice (oscillator + envelope
// via audio-rate param connection + pitch mod route), an FX delay
// graph, sampler zone/round-robin behaviour, user-assignable slot
// overrides, slice-map chopping (Stage 19), and the Stage 20 driver
// features (sprite animation validation, envelope stage writes).
#include "app/project_session.h"
#include "plugins/ntp_loader.h"
#include "plugins/ntp_voices.h"
#include "rt/rt_assert.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

std::string errors_joined(const std::vector<std::string>& errors) {
    std::string all;
    for (const std::string& e : errors) {
        all += e + "\n";
    }
    return all;
}

// Minimal 16-bit mono PCM WAV (mirrors the web fixture generator).
std::vector<std::uint8_t> make_wav(float freq, float seconds, std::uint32_t rate) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<std::uint8_t> bytes(44 + (static_cast<std::size_t>(frames) * 2));
    auto put32 = [&bytes](std::size_t at, std::uint32_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    };
    auto put16 = [&bytes](std::size_t at, std::uint16_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36 + (frames * 2));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        const auto v = static_cast<std::int16_t>(s * 20000.0F);
        put16(44 + (static_cast<std::size_t>(i) * 2), static_cast<std::uint16_t>(v));
    }
    return bytes;
}

// Multi-segment WAV: one pure tone per segment, so every grid slice of
// the result is spectrally distinguishable from its neighbours.
std::vector<std::uint8_t> make_segments_wav(const std::vector<float>& freqs, float seg_seconds,
                                            std::uint32_t rate) {
    const auto seg_frames = static_cast<std::uint32_t>(seg_seconds * static_cast<float>(rate));
    const std::uint32_t frames = seg_frames * static_cast<std::uint32_t>(freqs.size());
    std::vector<std::uint8_t> bytes = make_wav(1.0F, 0.0F, rate); // header only
    bytes.resize(44 + (static_cast<std::size_t>(frames) * 2));
    auto put32 = [&bytes](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes[at + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    put32(4, 36 + (frames * 2));
    put32(40, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float freq = freqs[i / seg_frames];
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        const auto v = static_cast<std::int16_t>(s * 20000.0F);
        bytes[44 + (static_cast<std::size_t>(i) * 2)] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[45 + (static_cast<std::size_t>(i) * 2)] =
            static_cast<std::uint8_t>((static_cast<std::uint16_t>(v) >> 8) & 0xFF);
    }
    return bytes;
}

// 16-bit mono PCM helpers shared by the transient/marker fixtures.
void wav_put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

void wav_put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void wav_write_header(std::vector<std::uint8_t>& b, std::uint32_t frames, std::uint32_t rate,
                      std::uint32_t riff_size) {
    std::memcpy(b.data(), "RIFF", 4);
    wav_put32(b, 4, riff_size);
    std::memcpy(b.data() + 8, "WAVEfmt ", 8);
    wav_put32(b, 16, 16);
    wav_put16(b, 20, 1);
    wav_put16(b, 22, 1);
    wav_put32(b, 24, rate);
    wav_put32(b, 28, rate * 2);
    wav_put16(b, 32, 2);
    wav_put16(b, 34, 16);
    std::memcpy(b.data() + 36, "data", 4);
    wav_put32(b, 40, frames * 2);
}

// Silence with decaying tone "plucks" (sharp attack, exponential decay,
// no abrupt cutoff) at the given sample offsets — one clean transient
// each, spectrally distinct by frequency. Feeds the "transients" slice
// path: the detector should place a boundary at each pluck.
std::vector<std::uint8_t> make_pluck_wav(const std::vector<std::uint32_t>& offsets,
                                         const std::vector<float>& freqs, float seconds,
                                         std::uint32_t rate) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<float> mono(frames, 0.0F);
    const auto len = static_cast<std::uint32_t>(0.15F * static_cast<float>(rate));
    for (std::size_t p = 0; p < offsets.size(); ++p) {
        for (std::uint32_t i = 0; i < len && offsets[p] + i < frames; ++i) {
            const float env = std::exp(-static_cast<float>(i) / (0.03F * static_cast<float>(rate)));
            mono[offsets[p] + i] += 0.7F * env *
                                    std::sin(2.0F * 3.14159265F * freqs[p] * static_cast<float>(i) /
                                             static_cast<float>(rate));
        }
    }
    std::vector<std::uint8_t> bytes(44 + (static_cast<std::size_t>(frames) * 2));
    wav_write_header(bytes, frames, rate, 36 + (frames * 2));
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(mono[i] * 20000.0F);
        wav_put16(bytes, 44 + (static_cast<std::size_t>(i) * 2), static_cast<std::uint16_t>(v));
    }
    return bytes;
}

// A multi-segment tone WAV (one distinct frequency per segment) carrying
// a 'cue ' chunk with a cue point at each interior segment boundary.
// Feeds the "markers" slice path: the loader should slice at the cues.
std::vector<std::uint8_t> make_cue_wav(const std::vector<float>& freqs, float seg_seconds,
                                       std::uint32_t rate) {
    const auto seg_frames = static_cast<std::uint32_t>(seg_seconds * static_cast<float>(rate));
    const auto nseg = static_cast<std::uint32_t>(freqs.size());
    const std::uint32_t frames = seg_frames * nseg;
    const std::uint32_t data_size = frames * 2;
    const std::uint32_t cue_count = nseg - 1; // interior boundaries only
    const std::uint32_t cue_size = 4 + (cue_count * 24);
    const std::uint32_t riff_size = 4 + (8 + 16) + (8 + data_size) + (8 + cue_size);
    std::vector<std::uint8_t> b(8 + riff_size);
    wav_write_header(b, frames, rate, riff_size);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float freq = freqs[i / seg_frames];
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        wav_put16(b, 44 + (static_cast<std::size_t>(i) * 2),
                  static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 20000.0F)));
    }
    const std::size_t off = 44 + data_size;
    std::memcpy(b.data() + off, "cue ", 4);
    wav_put32(b, off + 4, cue_size);
    wav_put32(b, off + 8, cue_count);
    for (std::uint32_t i = 0; i < cue_count; ++i) {
        const std::size_t rec = off + 12 + (static_cast<std::size_t>(i) * 24);
        const std::uint32_t boundary = (i + 1) * seg_frames;
        wav_put32(b, rec, i + 1);        // dwName
        wav_put32(b, rec + 4, boundary); // dwPosition
        std::memcpy(b.data() + rec + 8, "data", 4);
        wav_put32(b, rec + 12, 0);        // dwChunkStart
        wav_put32(b, rec + 16, 0);        // dwBlockStart
        wav_put32(b, rec + 20, boundary); // dwSampleOffset — the slice boundary
    }
    return b;
}

std::vector<std::uint8_t>
make_zip(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0) == MZ_TRUE);
    for (const auto& [name, data] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                                      MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
    }
    void* buf = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size) == MZ_TRUE);
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(buf),
                                  static_cast<std::uint8_t*>(buf) + size);
    mz_zip_writer_end(&zip);
    mz_free(buf); // finalize_heap_archive transfers ownership to us
    return out;
}

std::vector<std::uint8_t> to_bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

float goertzel(const std::vector<float>& mono, std::uint32_t rate, float freq) {
    const std::size_t n = mono.size();
    const double k = std::round(static_cast<double>(n) * freq / rate);
    const double w = 2.0 * 3.14159265358979 * k / static_cast<double>(n);
    const double c = 2.0 * std::cos(w);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : mono) {
        s0 = x + (c * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(std::max(0.0, (s1 * s1) + (s2 * s2) - (c * s1 * s2))) /
                              (static_cast<double>(n) / 2.0));
}

} // namespace

TEST_CASE("manifest validation is strict and collects every error", "[ntp]") {
    ntp::Manifest manifest;
    std::vector<std::string> errors;
    const std::string bad = R"({
      "id": "test.bad", "type": "fx",
      "params": [
        {"key": "a", "min": 1, "max": 0, "default": 2},
        {"key": "a", "min": 0, "max": 1, "default": 0}
      ],
      "graph": {
        "nodes": [
          {"id": "w", "type": "worklet"},
          {"id": "n", "type": "native_stage"},
          {"id": "x", "type": "mystery"}
        ],
        "connections": [{"from": "ghost", "to": "output"}]
      },
      "requires": ["webview", "userSamples"]
    })";
    CHECK_FALSE(nt::plugins::parse_ntp_manifest(bad, manifest, errors));
    const std::string all = errors_joined(errors);
    CHECK(all.find("missing or unsupported \"ntp\"") != std::string::npos);
    CHECK(all.find("missing plugin name") != std::string::npos);
    CHECK(all.find("min must be < max") != std::string::npos);
    CHECK(all.find("default outside range") != std::string::npos);
    CHECK(all.find("duplicate parameter key") != std::string::npos);
    CHECK(all.find("worklet") != std::string::npos);
    CHECK(all.find("native_stage needs a stage") != std::string::npos);
    CHECK(all.find("unknown type \"mystery\"") != std::string::npos);
    CHECK(all.find("unknown endpoint \"ghost\"") != std::string::npos);
    // "webview" is refused; "userSamples" is the one native capability
    // (Stage 19) and must NOT be reported.
    CHECK(all.find("unknown capability requirement \"webview\"") != std::string::npos);
    CHECK(all.find("unknown capability requirement \"userSamples\"") == std::string::npos);
}

TEST_CASE("archive loads through a real ZIP and plays a synth voice", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.synth", "name": "TEST SYNTH", "type": "instrument",
      "params": [
        {"key": "amp.gain", "label": "AMP", "min": 0, "max": 1, "default": 0}
      ],
      "voices": 4,
      "graph": {
        "nodes": [
          {"id": "osc", "type": "oscillator", "scope": "voice",
           "oscType": "sine", "frequency": 0, "gain": 0.5},
          {"id": "env", "type": "envelope", "scope": "voice", "release": 0.05,
           "envStages": [{"target": 1, "time": 0.005}]},
          {"id": "amp", "type": "gain", "scope": "voice", "gain": 0}
        ],
        "connections": [
          {"from": "osc", "to": "amp"},
          {"from": "env", "to": "amp", "toParam": "gain"},
          {"from": "amp", "to": "voiceOut"}
        ],
        "modRoutes": [
          {"source": "pitch", "targets": [{"target": "osc.frequency", "depth": 1}]}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({{"plugin.json", to_bytes(manifest_json)}});

    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);
    CHECK(plugin->manifest.type == ntp::PluginType::kInstrument);
    // Implicit main out was injected.
    REQUIRE(plugin->manifest.outputs.size() == 1);
    CHECK(plugin->manifest.outputs[0].id == "main");

    nt::plugins::NtpInstance instance(*plugin, kRate);
    instance.note_on(69, 1.0F); // A4 → osc frequency via pitch mod route

    std::vector<float> mono;
    std::array<float, 256> block{};
    for (int b = 0; b < 40; ++b) { // ~106ms
        instance.process(nullptr, block.data(), 128);
        for (int i = 0; i < 128; ++i) {
            mono.push_back(block[static_cast<std::size_t>(i) * 2]);
        }
    }
    // The held voice puts a strong 440 Hz line in the output.
    const float at_440 = goertzel(mono, kRate, 440.0F);
    const float at_620 = goertzel(mono, kRate, 620.0F);
    CHECK(at_440 > 0.1F);
    CHECK(at_440 > at_620 * 5.0F);

    // Release: after gate-off the envelope decays and the voice frees;
    // output falls to silence.
    instance.note_off(69);
    for (int b = 0; b < 60; ++b) { // ~160ms >> 50ms release
        instance.process(nullptr, block.data(), 128);
    }
    float tail_peak = 0.0F;
    instance.process(nullptr, block.data(), 128);
    for (const float v : block) {
        tail_peak = std::max(tail_peak, std::abs(v));
    }
    CHECK(tail_peak < 1e-3F);
}

TEST_CASE("fx graph delays the input by the configured time", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.delay", "name": "TEST DELAY", "type": "fx",
      "graph": {
        "nodes": [{"id": "d", "type": "delay", "delayTime": 0.01, "maxDelay": 0.1}],
        "connections": [
          {"from": "input", "to": "d"},
          {"from": "d", "to": "output"}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({{"plugin.json", to_bytes(manifest_json)}});
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);
    std::array<float, 256> in{};
    std::array<float, 256> out{};
    in[0] = 1.0F;
    in[1] = 1.0F;

    // 0.01s at 48k = 480 frames = 3 blocks + 96.
    std::vector<float> collected;
    for (int b = 0; b < 6; ++b) {
        instance.process(in.data(), out.data(), 128);
        for (int i = 0; i < 128; ++i) {
            collected.push_back(out[static_cast<std::size_t>(i) * 2]);
        }
        in[0] = 0.0F;
        in[1] = 0.0F;
    }
    std::size_t peak_at = 0;
    float peak = 0.0F;
    for (std::size_t i = 0; i < collected.size(); ++i) {
        if (std::abs(collected[i]) > peak) {
            peak = std::abs(collected[i]);
            peak_at = i;
        }
    }
    CHECK(peak > 0.5F);
    CHECK(peak_at >= 478);
    CHECK(peak_at <= 482);
}

TEST_CASE("sampler zones map keys and rotate round-robin groups", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.kit", "name": "TEST KIT", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler", "polyphony": 8, "zones": [
          {"file": "a.wav", "rootKey": 60, "keyRange": {"lo": 60, "hi": 60},
           "velocityRange": {"lo": 1, "hi": 127}, "roundRobinGroup": "rr",
           "loop": "none", "loopStart": 0, "loopEnd": 0, "startOffset": 0, "duration": 0},
          {"file": "b.wav", "rootKey": 60, "keyRange": {"lo": 60, "hi": 60},
           "velocityRange": {"lo": 1, "hi": 127}, "roundRobinGroup": "rr",
           "loop": "none", "loopStart": 0, "loopEnd": 0, "startOffset": 0, "duration": 0}
        ]}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"a.wav", make_wav(500.0F, 0.05F, kRate)},
        {"b.wav", make_wav(900.0F, 0.05F, kRate)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);
    CHECK(plugin->samples.size() == 2);

    nt::plugins::NtpInstance instance(*plugin, kRate);

    auto render_trigger = [&instance]() {
        instance.note_on(60, 1.0F);
        std::vector<float> mono;
        std::array<float, 256> block{};
        for (int b = 0; b < 18; ++b) { // ~48ms
            instance.process(nullptr, block.data(), 128);
            for (int i = 0; i < 128; ++i) {
                mono.push_back(block[static_cast<std::size_t>(i) * 2]);
            }
        }
        instance.note_off(60);
        // Drain the tail fully so triggers don't overlap.
        for (int b = 0; b < 20; ++b) {
            instance.process(nullptr, block.data(), 128);
        }
        return mono;
    };

    const std::vector<float> first = render_trigger();
    const std::vector<float> second = render_trigger();
    // Round-robin: first trigger plays the 500 Hz zone, second the 900.
    CHECK(goertzel(first, kRate, 500.0F) > goertzel(first, kRate, 900.0F) * 3.0F);
    CHECK(goertzel(second, kRate, 900.0F) > goertzel(second, kRate, 500.0F) * 3.0F);

    // Out-of-range notes are silent.
    instance.note_on(72, 1.0F);
    std::array<float, 256> block{};
    instance.process(nullptr, block.data(), 128);
    float peak = 0.0F;
    for (const float v : block) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak < 1e-5F);
}

// ── Stage 19: sampler platform ───────────────────────────────────────

namespace {

// Renders `blocks` × 128 frames under the RT allocation guard (debug
// builds abort on any audio-thread allocation in the trigger/render
// paths) and returns the left channel.
std::vector<float> render_blocks(nt::plugins::NtpInstance& instance, int blocks) {
    std::vector<float> mono;
    mono.reserve(static_cast<std::size_t>(blocks) * 128);
    std::array<float, 256> block{};
    for (int b = 0; b < blocks; ++b) {
        {
            [[maybe_unused]] const nt::rt::RtScope rt_scope;
            instance.process(nullptr, block.data(), 128);
        }
        for (int i = 0; i < 128; ++i) {
            mono.push_back(block[static_cast<std::size_t>(i) * 2]);
        }
    }
    return mono;
}

void rt_note_on(nt::plugins::NtpInstance& instance, int note) {
    [[maybe_unused]] const nt::rt::RtScope rt_scope;
    instance.note_on(note, 1.0F);
}

void rt_note_off(nt::plugins::NtpInstance& instance, int note) {
    [[maybe_unused]] const nt::rt::RtScope rt_scope;
    instance.note_off(note);
}

} // namespace

TEST_CASE("slot and slice validation failures are collected, not thrown", "[ntp]") {
    ntp::Manifest manifest;
    std::vector<std::string> errors;
    // Two user-assignable zones sharing a slot id, one with no slot id
    // or fallback, no "userSamples" capability; a slice map with a bad
    // trigger mode, an inverted slice, an unknown autoDetect form, and a
    // slot-sourced map referencing a slot that does not exist.
    const std::string bad = R"({
      "ntp": 1, "id": "test.badslots", "name": "BAD", "type": "instrument",
      "graph": {
        "nodes": [
          {"id": "s1", "type": "sampler", "zones": [
            {"file": "a.wav", "userAssignable": true, "slotId": "kick",
             "fallbackFile": "a.wav"},
            {"file": "b.wav", "userAssignable": true, "slotId": "kick",
             "fallbackFile": "b.wav"},
            {"file": "c.wav", "userAssignable": true}
          ]},
          {"id": "s2", "type": "sampler",
           "sliceMap": {"source": "break.wav", "triggerMode": "sideways",
                        "slices": [{"start": 0.5, "end": 0.1}]}},
          {"id": "s3", "type": "sampler",
           "sliceMap": {"source": "break.wav", "autoDetect": "spectral-magic"}},
          {"id": "s4", "type": "sampler",
           "sliceMap": {"source": "slotId:ghost", "autoDetect": "grid:8"}}
        ]
      }
    })";
    CHECK_FALSE(nt::plugins::parse_ntp_manifest(bad, manifest, errors));
    const std::string all = errors_joined(errors);
    CHECK(all.find("duplicate slot id \"kick\"") != std::string::npos);
    CHECK(all.find("needs a slotId") != std::string::npos);
    CHECK(all.find("needs a fallbackFile") != std::string::npos);
    CHECK(all.find("needs \"userSamples\" in requires[]") != std::string::npos);
    CHECK(all.find("triggerMode must be") != std::string::npos);
    CHECK(all.find("needs 0 <= start < end") != std::string::npos);
    CHECK(all.find("autoDetect must be") != std::string::npos);
    CHECK(all.find("unknown slot \"ghost\"") != std::string::npos);
}

TEST_CASE("user slot override swaps the zone buffer and clears to fallback", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.slotkit", "name": "SLOT KIT", "type": "instrument",
      "requires": ["userSamples"],
      "graph": {
        "nodes": [{"id": "s", "type": "sampler", "zones": [
          {"userAssignable": true, "slotId": "voice", "slotLabel": "VOICE",
           "fallbackFile": "fallback.wav", "rootKey": 60,
           "keyRange": {"lo": 60, "hi": 60}}
        ]}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"fallback.wav", make_wav(500.0F, 0.1F, kRate)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);
    auto render_note = [&instance]() {
        rt_note_on(instance, 60);
        std::vector<float> mono = render_blocks(instance, 18);
        rt_note_off(instance, 60);
        render_blocks(instance, 24); // drain
        return mono;
    };

    // Baked fallback plays before any override.
    const std::vector<float> before = render_note();
    CHECK(goertzel(before, kRate, 500.0F) > goertzel(before, kRate, 900.0F) * 3.0F);

    // Override: decode a 900 Hz user sample through the standard path.
    const std::vector<std::uint8_t> user_wav = make_wav(900.0F, 0.1F, kRate);
    std::string format;
    std::string decode_error;
    std::shared_ptr<nt::audio::SampleBuffer> user_buffer = nt::audio::load_sample_memory(
        user_wav.data(), user_wav.size(), kRate, format, decode_error);
    REQUIRE(user_buffer != nullptr);
    nt::plugins::SlotOverride entry;
    entry.hash = "sha256:test";
    entry.name = "user.wav";
    entry.buffer = user_buffer;
    CHECK(instance.set_slot_override("voice", std::move(entry)) == nullptr);

    const std::vector<float> after = render_note();
    CHECK(goertzel(after, kRate, 900.0F) > goertzel(after, kRate, 500.0F) * 3.0F);
    REQUIRE(instance.slot_overrides().contains("voice"));

    // Clear reverts to the fallback and hands the buffer back for
    // retirement.
    CHECK(instance.clear_slot_override("voice") == user_buffer);
    const std::vector<float> reverted = render_note();
    CHECK(goertzel(reverted, kRate, 500.0F) > goertzel(reverted, kRate, 900.0F) * 3.0F);
}

TEST_CASE("slice map grid-chops a break with one-shot and gated triggers", "[ntp]") {
    // Eight 50 ms segments, mutually non-harmonic tones — every slice
    // is spectrally distinguishable from its neighbours.
    const std::vector<float> freqs = {380.0F, 500.0F, 620.0F,  740.0F,
                                      860.0F, 980.0F, 1100.0F, 1220.0F};
    const std::vector<std::uint8_t> break_wav = make_segments_wav(freqs, 0.05F, kRate);
    auto manifest_json = [](const char* mode) {
        return std::string(R"({
          "ntp": 1, "id": "test.chop", "name": "CHOP", "type": "instrument",
          "graph": {
            "nodes": [{"id": "s", "type": "sampler",
              "sliceMap": {"source": "break.wav", "autoDetect": "grid:8",
                           "triggerMode": ")") +
               mode + R"("}}],
            "connections": [{"from": "s", "to": "output"}]
          }
        })";
    };

    SECTION("one-shot plays out past noteOff; default notes map 36+index") {
        const std::vector<std::uint8_t> zip = make_zip({
            {"plugin.json", to_bytes(manifest_json("oneShot"))},
            {"break.wav", break_wav},
        });
        std::vector<std::string> errors;
        auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
        INFO(errors_joined(errors));
        REQUIRE(plugin != nullptr);
        REQUIRE(plugin->manifest.graph.nodes[0].slice_map.has_value());
        CHECK(plugin->manifest.graph.nodes[0].slice_map->slices.size() == 8);

        nt::plugins::NtpInstance instance(*plugin, kRate);
        // Note 39 = 36 + 3 → slice 3.
        rt_note_on(instance, 39);
        const std::vector<float> held = render_blocks(instance, 4); // 512 fr
        rt_note_off(instance, 39);
        // One-shot: the slice keeps sounding after noteOff...
        const std::vector<float> tail = render_blocks(instance, 10);
        CHECK(goertzel(held, kRate, freqs[3]) > goertzel(held, kRate, freqs[2]) * 3.0F);
        CHECK(goertzel(tail, kRate, freqs[3]) > 0.1F);
        // ...but ends at the slice boundary (2400 frames total).
        render_blocks(instance, 6);
        const std::vector<float> ended = render_blocks(instance, 4);
        float peak = 0.0F;
        for (const float v : ended) {
            peak = std::max(peak, std::abs(v));
        }
        CHECK(peak < 1e-4F);
    }

    SECTION("gated cuts the voice on noteOff") {
        const std::vector<std::uint8_t> zip = make_zip({
            {"plugin.json", to_bytes(manifest_json("gated"))},
            {"break.wav", break_wav},
        });
        std::vector<std::string> errors;
        auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
        INFO(errors_joined(errors));
        REQUIRE(plugin != nullptr);

        nt::plugins::NtpInstance instance(*plugin, kRate);
        rt_note_on(instance, 36);
        const std::vector<float> held = render_blocks(instance, 4);
        CHECK(goertzel(held, kRate, freqs[0]) > 0.1F);
        rt_note_off(instance, 36);
        const std::vector<float> cut = render_blocks(instance, 4);
        float peak = 0.0F;
        for (const float v : cut) {
            peak = std::max(peak, std::abs(v));
        }
        CHECK(peak < 1e-4F);
    }
}

TEST_CASE("slice choke groups cut and round-robin groups rotate", "[ntp]") {
    // 0.0-0.4 s at 500 Hz, 0.4-0.8 s at 900 Hz. Choke pair: notes
    // 60/61 play the long halves; RR pair: note 62 alternates two
    // short windows, one in each half.
    const std::vector<std::uint8_t> duo_wav = make_segments_wav({500.0F, 900.0F}, 0.4F, kRate);
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.chopgroups", "name": "CHOP GROUPS", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler",
          "sliceMap": {"source": "duo.wav", "triggerMode": "oneShot", "slices": [
            {"start": 0.0, "end": 0.4, "note": 60, "choke": "grp"},
            {"start": 0.4, "end": 0.8, "note": 61, "choke": "grp"},
            {"start": 0.0, "end": 0.05, "note": 62, "roundRobinGroup": "rr"},
            {"start": 0.4, "end": 0.45, "note": 62, "roundRobinGroup": "rr"}
          ]}}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"duo.wav", duo_wav},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);

    // Choke: slice 60 (500 Hz) is playing when slice 61 (900 Hz)
    // fires in the same group — the capture window after the second
    // trigger must not contain 500 Hz.
    rt_note_on(instance, 60);
    render_blocks(instance, 4);
    rt_note_on(instance, 61);
    const std::vector<float> choked = render_blocks(instance, 12);
    CHECK(goertzel(choked, kRate, 900.0F) > 0.1F);
    CHECK(goertzel(choked, kRate, 900.0F) > goertzel(choked, kRate, 500.0F) * 3.0F);
    render_blocks(instance, 160); // drain both long slices fully

    // Round-robin: successive note-62 triggers alternate the two
    // short windows.
    rt_note_on(instance, 62);
    const std::vector<float> first = render_blocks(instance, 18);
    render_blocks(instance, 6); // past the 0.05 s slice end
    rt_note_on(instance, 62);
    const std::vector<float> second = render_blocks(instance, 18);
    CHECK(goertzel(first, kRate, 500.0F) > goertzel(first, kRate, 900.0F) * 3.0F);
    CHECK(goertzel(second, kRate, 900.0F) > goertzel(second, kRate, 500.0F) * 3.0F);
}

// ── Stage 27: transient / marker slicing ─────────────────────────────

TEST_CASE("sliceMap autoDetect \"transients\" slices a break at its onsets", "[ntp]") {
    // Four decaying plucks at distinct frequencies, one every 0.25 s.
    // The pluck at frame 0 has no preceding STFT frame, so the detector
    // reports the three interior onsets and the loader forces slice 0 to
    // start at 0 — four slices, one pluck each (note 36 + index).
    const std::vector<std::uint32_t> offsets = {0, 12000, 24000, 36000};
    const std::vector<float> freqs = {400.0F, 700.0F, 1000.0F, 1400.0F};
    const std::vector<std::uint8_t> break_wav = make_pluck_wav(offsets, freqs, 1.0F, kRate);
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.transients", "name": "TRANSIENTS", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler",
          "sliceMap": {"source": "break.wav", "autoDetect": "transients"}}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"break.wav", break_wav},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->manifest.graph.nodes[0].slice_map.has_value());
    CHECK(plugin->warnings.empty()); // transients were found, no fallback
    const auto& slices = plugin->manifest.graph.nodes[0].slice_map->slices;
    REQUIRE(slices.size() == 4);
    // Slice 0 always starts at the head; boundaries land near the plucks.
    CHECK(slices[0].start == 0.0);
    CHECK(slices[1].start == Catch::Approx(0.25).margin(0.02));
    CHECK(slices[2].start == Catch::Approx(0.50).margin(0.02));
    CHECK(slices[3].start == Catch::Approx(0.75).margin(0.02));

    nt::plugins::NtpInstance instance(*plugin, kRate);
    auto slice_tone = [&](int note) {
        rt_note_on(instance, note);
        std::vector<float> mono = render_blocks(instance, 40); // ~107 ms
        rt_note_off(instance, note);
        render_blocks(instance, 30); // drain
        return mono;
    };
    // note 36 → slice 0 (400 Hz), note 38 → slice 2 (1000 Hz).
    const std::vector<float> s0 = slice_tone(36);
    CHECK(goertzel(s0, kRate, 400.0F) > goertzel(s0, kRate, 1000.0F) * 3.0F);
    const std::vector<float> s2 = slice_tone(38);
    CHECK(goertzel(s2, kRate, 1000.0F) > goertzel(s2, kRate, 400.0F) * 3.0F);
}

TEST_CASE("sliceMap autoDetect \"transients\" falls back on a silent source", "[ntp]") {
    // Silence carries no transients — honest whole-sample slice + warning.
    const std::vector<std::uint8_t> quiet = make_wav(0.0F, 0.5F, kRate); // amp 0
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.silent", "name": "SILENT", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler",
          "sliceMap": {"source": "q.wav", "autoDetect": "transients"}}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip =
        make_zip({{"plugin.json", to_bytes(manifest_json)}, {"q.wav", quiet}});
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr); // fallback loads, does not refuse
    const auto& slices = plugin->manifest.graph.nodes[0].slice_map->slices;
    CHECK(slices.size() == 1);
    CHECK(slices[0].start == 0.0);
    CHECK(slices[0].end == Catch::Approx(0.5));
    bool warned = false;
    for (const std::string& w : plugin->warnings) {
        warned = warned || w.find("found no transients") != std::string::npos;
    }
    CHECK(warned);
}

TEST_CASE("sliceMap autoDetect \"markers\" slices at WAV cue points", "[ntp]") {
    // Four 0.25 s tone segments with cue points at each interior
    // boundary — the loader slices exactly at the cues.
    const std::vector<float> freqs = {400.0F, 700.0F, 1000.0F, 1400.0F};
    const std::vector<std::uint8_t> cue_wav = make_cue_wav(freqs, 0.25F, kRate);
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.markers", "name": "MARKERS", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler",
          "sliceMap": {"source": "cue.wav", "autoDetect": "markers"}}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip =
        make_zip({{"plugin.json", to_bytes(manifest_json)}, {"cue.wav", cue_wav}});
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);
    REQUIRE(plugin->manifest.graph.nodes[0].slice_map.has_value());
    const auto& slices = plugin->manifest.graph.nodes[0].slice_map->slices;
    REQUIRE(slices.size() == 4);
    CHECK(slices[0].start == 0.0);
    CHECK(slices[1].start == Catch::Approx(0.25));
    CHECK(slices[2].start == Catch::Approx(0.50));
    CHECK(slices[3].start == Catch::Approx(0.75));

    nt::plugins::NtpInstance instance(*plugin, kRate);
    auto slice_tone = [&](int note) {
        rt_note_on(instance, note);
        std::vector<float> mono = render_blocks(instance, 40); // capture ~107 ms
        rt_note_off(instance, note);
        render_blocks(instance, 110); // drain: a 0.25 s oneShot slice runs to its end
        return mono;
    };
    const std::vector<float> s1 = slice_tone(37); // slice 1 → 700 Hz
    CHECK(goertzel(s1, kRate, 700.0F) > goertzel(s1, kRate, 400.0F) * 3.0F);
    const std::vector<float> s3 = slice_tone(39); // slice 3 → 1400 Hz
    CHECK(goertzel(s3, kRate, 1400.0F) > goertzel(s3, kRate, 700.0F) * 3.0F);
}

TEST_CASE("sliceMap autoDetect \"markers\" on a source without cues is refused", "[ntp]") {
    // A plain WAV (no cue chunk) cannot satisfy "markers" — a collected
    // error, not a silent one-slice fallback (strict-load philosophy).
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.nocue", "name": "NOCUE", "type": "instrument",
      "graph": {
        "nodes": [{"id": "s", "type": "sampler",
          "sliceMap": {"source": "plain.wav", "autoDetect": "markers"}}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip(
        {{"plugin.json", to_bytes(manifest_json)}, {"plain.wav", make_wav(440.0F, 0.2F, kRate)}});
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    CHECK(plugin == nullptr);
    CHECK(errors_joined(errors).find("has no cue points") != std::string::npos);
}

// ── Stage 20: convolver uncap ────────────────────────────────────────

TEST_CASE("convolver renders impulses beyond the old 2048-tap cap", "[ntp]") {
    // 0.15 s of 500 Hz as the impulse: 7200 taps — 3.5x the removed
    // kMaxImpulseFrames cap. A click must play the whole impulse back,
    // so energy far past frame 2048 (where the truncated FIR fell
    // silent) proves the uncap, and the 500 Hz line is the tilt check.
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.conv", "name": "TEST CONV", "type": "fx",
      "graph": {
        "nodes": [{"id": "c", "type": "convolver", "impulse": "ir.wav"}],
        "connections": [
          {"from": "input", "to": "c"},
          {"from": "c", "to": "output"}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"ir.wav", make_wav(500.0F, 0.15F, kRate)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);
    std::array<float, 256> in{};
    std::array<float, 256> out{};
    in[0] = 1.0F;
    in[1] = 1.0F;
    std::vector<float> mono;
    for (int b = 0; b < 60; ++b) { // 7680 frames > the 7200-tap tail
        {
            [[maybe_unused]] const nt::rt::RtScope rt_scope;
            instance.process(in.data(), out.data(), 128);
        }
        for (int i = 0; i < 128; ++i) {
            mono.push_back(out[static_cast<std::size_t>(i) * 2]);
        }
        in[0] = 0.0F;
        in[1] = 0.0F;
    }

    // Expected spectral tilt over the whole response.
    CHECK(goertzel(mono, kRate, 500.0F) > goertzel(mono, kRate, 900.0F) * 5.0F);
    // Non-silence well past the old cap (frames 4096..7168).
    const std::vector<float> tail(mono.begin() + 4096, mono.begin() + 7168);
    CHECK(goertzel(tail, kRate, 500.0F) > 0.005F);
    CHECK(goertzel(tail, kRate, 500.0F) > goertzel(tail, kRate, 900.0F) * 5.0F);
    // The impulse does end: silence after 7200 frames.
    float end_peak = 0.0F;
    for (std::size_t i = 7400; i < mono.size(); ++i) {
        end_peak = std::max(end_peak, std::abs(mono[i]));
    }
    CHECK(end_peak < 1e-4F);
}

// ── Stage 20: sprite animations + envelope stage editing ─────────────

namespace {

// RGBA sprite sheet as a real PNG (miniz's tdefl writer): each 4x4
// cell filled with a distinct colour so frames are distinguishable.
std::vector<std::uint8_t> make_sheet_png(int width, int height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at =
                ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                 static_cast<std::size_t>(x)) *
                4;
            const int cell = (x / 4) + ((y / 4) * (width / 4));
            rgba[at] = static_cast<std::uint8_t>(40 * (cell + 1));
            rgba[at + 1] = static_cast<std::uint8_t>(255 - (40 * cell));
            rgba[at + 2] = 128;
            rgba[at + 3] = 255;
        }
    }
    std::size_t png_size = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), width, height, 4, &png_size);
    REQUIRE(png != nullptr);
    std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(png),
                                    static_cast<std::uint8_t*>(png) + png_size);
    mz_free(png);
    return bytes;
}

// Instrument with a voice envelope driving an amp — the envelope
// editor's write target.
std::vector<std::uint8_t> make_env_synth_zip() {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.envsynth", "name": "ENV SYNTH", "type": "instrument",
      "voices": 4,
      "graph": {
        "nodes": [
          {"id": "osc", "type": "oscillator", "scope": "voice",
           "oscType": "sine", "frequency": 0, "gain": 0.5},
          {"id": "env", "type": "envelope", "scope": "voice", "release": 0.05,
           "envStages": [{"target": 1, "time": 0.002}, {"target": 0.6, "time": 0.05}]},
          {"id": "amp", "type": "gain", "scope": "voice", "gain": 0}
        ],
        "connections": [
          {"from": "osc", "to": "amp"},
          {"from": "env", "to": "amp", "toParam": "gain"},
          {"from": "amp", "to": "voiceOut"}
        ],
        "modRoutes": [
          {"source": "pitch", "targets": [{"target": "osc.frequency", "depth": 1}]}
        ]
      },
      "ui": {"controls": [{"type": "envelope_editor", "node": "env"}]}
    })";
    return make_zip({{"plugin.json", to_bytes(manifest_json)}});
}

std::filesystem::path write_temp_file(const char* name, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/file seam
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

} // namespace

TEST_CASE("sprite animations load with frame lists, fps default and interaction keys", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.sprite", "name": "SPRITE FX", "type": "fx",
      "params": [{"key": "g", "label": "G", "min": 0, "max": 1, "default": 0.5}],
      "graph": {
        "nodes": [{"id": "amp", "type": "gain"}],
        "connections": [
          {"from": "input", "to": "amp"},
          {"from": "amp", "to": "output"}
        ]
      },
      "ui": {"controls": [
        {"type": "sprite", "asset": "sheet.png", "frameW": 4, "frameH": 4,
         "animations": [
           {"name": "idle", "frames": [0, 1], "loop": true},
           {"name": "hit", "frames": [1, 0, 1], "fps": 24}
         ]},
        {"type": "knob", "parameter": "g", "animation": "hit"}
      ]}
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"sheet.png", make_sheet_png(8, 4)}, // 2 cells of 4x4
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    REQUIRE(plugin->manifest.ui.controls.size() == 2);
    const ntp::UiControl& sprite = plugin->manifest.ui.controls[0];
    CHECK(sprite.frame_width == 4);
    CHECK(sprite.frame_height == 4);
    REQUIRE(sprite.animations.size() == 2);
    CHECK(sprite.animations[0].name == "idle");
    CHECK(sprite.animations[0].frames == std::vector<int>({0, 1}));
    CHECK(sprite.animations[0].fps == 10.0); // web DEFAULT_SPRITE_FPS
    CHECK(sprite.animations[0].loop);
    CHECK(sprite.animations[1].name == "hit");
    CHECK(sprite.animations[1].fps == 24.0);
    CHECK_FALSE(sprite.animations[1].loop);
    CHECK(plugin->manifest.ui.controls[1].animation == "hit");
    // The sheet decoded: two distinguishable 4x4 frames.
    REQUIRE(plugin->images.contains("sheet.png"));
    const auto& image = plugin->images.at("sheet.png");
    CHECK(image.width == 8);
    CHECK(image.height == 4);
    CHECK(image.rgba[0] != image.rgba[4LL * 4]); // frame 0 vs frame 1 texel
}

TEST_CASE("sprite animation validation failures are collected, not thrown", "[ntp]") {
    SECTION("shape errors at manifest level") {
        const std::string bad = R"({
          "ntp": 1, "id": "test.badsprite", "name": "BAD", "type": "fx",
          "params": [{"key": "g", "label": "G", "min": 0, "max": 1, "default": 0}],
          "graph": {"nodes": [{"id": "amp", "type": "gain"}],
                    "connections": [{"from": "input", "to": "amp"},
                                    {"from": "amp", "to": "output"}]},
          "ui": {"controls": [
            {"type": "sprite", "asset": "a.png",
             "animations": [
               {"frames": [0]},
               {"name": "dup", "frames": []},
               {"name": "dup", "frames": [-1], "fps": 0}
             ]},
            {"type": "knob", "parameter": "g", "animation": "ghost"},
            {"type": "label", "label": "L", "animations": []}
          ]}
        })";
        ntp::Manifest manifest;
        std::vector<std::string> errors;
        CHECK_FALSE(nt::plugins::parse_ntp_manifest(bad, manifest, errors));
        const std::string all = errors_joined(errors);
        CHECK(all.find("sprite animation without a name") != std::string::npos);
        CHECK(all.find("needs a non-empty frames[]") != std::string::npos);
        CHECK(all.find("negative frame index") != std::string::npos);
        CHECK(all.find("needs fps > 0") != std::string::npos);
        CHECK(all.find("duplicate sprite animation name \"dup\"") != std::string::npos);
        CHECK(all.find("unknown animation \"ghost\"") != std::string::npos);
        CHECK(all.find("only valid on sprite controls") != std::string::npos);
        CHECK(all.find("no frameW/frameH grid") != std::string::npos);
    }

    SECTION("frame indices bound to the decoded sheet grid") {
        const std::string manifest_json = R"({
          "ntp": 1, "id": "test.overrun", "name": "OVERRUN", "type": "fx",
          "graph": {"nodes": [{"id": "amp", "type": "gain"}],
                    "connections": [{"from": "input", "to": "amp"},
                                    {"from": "amp", "to": "output"}]},
          "ui": {"controls": [
            {"type": "sprite", "asset": "sheet.png", "frameW": 4, "frameH": 4,
             "animations": [{"name": "spin", "frames": [0, 5], "loop": true}]}
          ]}
        })";
        const std::vector<std::uint8_t> zip = make_zip({
            {"plugin.json", to_bytes(manifest_json)},
            {"sheet.png", make_sheet_png(8, 4)}, // capacity 2: frames 0..1
        });
        std::vector<std::string> errors;
        auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
        CHECK(plugin == nullptr);
        CHECK(errors_joined(errors).find("frame 5 outside the 2-frame sheet") != std::string::npos);
    }
}

TEST_CASE("envelope stage writes clamp and reshape the attack", "[ntp]") {
    const std::vector<std::uint8_t> zip = make_env_synth_zip();
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    // ~21 ms of held note; the first-stage attack time decides how
    // much level accumulates inside the window.
    auto render_peak = [](nt::plugins::NtpInstance& instance) {
        std::array<float, 256> block{};
        float peak = 0.0F;
        instance.note_on(69, 1.0F);
        for (int b = 0; b < 8; ++b) {
            instance.process(nullptr, block.data(), 128);
            for (const float v : block) {
                peak = std::max(peak, std::abs(v));
            }
        }
        return peak;
    };

    float fast_peak = 0.0F;
    {
        nt::plugins::NtpInstance instance(*plugin, kRate);
        // Out-of-range writes clamp: target 0..1, time >= 1 ms.
        CHECK(instance.set_env_stage("env", 0, 3.0, 0.0));
        // Unknown node / non-envelope node / bad index are refused.
        CHECK_FALSE(instance.set_env_stage("ghost", 0, 0.5, 0.1));
        CHECK_FALSE(instance.set_env_stage("amp", 0, 0.5, 0.1));
        CHECK_FALSE(instance.set_env_stage("env", 2, 0.5, 0.1));
        CHECK_FALSE(instance.set_env_stage("env", -1, 0.5, 0.1));
        // The edit is a per-instance override, clamped (target 0..1,
        // time >= 1 ms); the shared manifest stays at its authored value.
        const std::vector<ntp::EnvelopeStage> eff = instance.effective_env_stages("env");
        REQUIRE(eff.size() == 2);
        CHECK(eff[0].target == 1.0);
        CHECK(eff[0].time == 0.001);
        CHECK(plugin->manifest.graph.nodes[1].env_stages[0].time == 0.002); // manifest untouched
        fast_peak = render_peak(instance);
    }

    float slow_peak = 0.0F;
    {
        nt::plugins::NtpInstance instance(*plugin, kRate);
        CHECK(instance.set_env_stage("env", 0, 1.0, 0.2)); // 200 ms attack
        CHECK(instance.effective_env_stages("env")[0].time == 0.2);
        CHECK(plugin->manifest.graph.nodes[1].env_stages[0].time == 0.002); // still authored
        slow_peak = render_peak(instance);
    }

    // 1 ms attack reaches full level inside the window; 200 ms barely
    // starts.
    CHECK(fast_peak > 0.4F);
    CHECK(slow_peak < fast_peak * 0.5F);
}

TEST_CASE("session envelope commit routes through the structural republish", "[ntp]") {
    const auto path = write_temp_file("nt_env_edit.ntins", make_env_synth_zip());
    nt::audio::AudioEngine audio; // not started — device-independent
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_plugin_file(path) == "test.envsynth");
    const std::string node_id = session.add_plugin_node("test.envsynth");
    REQUIRE_FALSE(node_id.empty());

    // The exact commit path the envelope editor's drag release takes.
    REQUIRE(session.set_plugin_env_stage(node_id, "env", 1, 1.4, 0.0002));
    const nt::plugins::NtpInstance* instance = session.plugin_instance(node_id);
    REQUIRE(instance != nullptr);
    // Per-instance override, clamped; the shared manifest is untouched.
    const std::vector<ntp::EnvelopeStage> eff = instance->effective_env_stages("env");
    REQUIRE(eff.size() == 2);
    CHECK(eff[1].target == 1.0);                                           // clamped from 1.4
    CHECK(eff[1].time == 0.001);                                           // clamped from 0.2 ms
    CHECK(eff[0].time == 0.002);                                           // untouched neighbour
    CHECK(instance->manifest().graph.nodes[1].env_stages[1].time == 0.05); // manifest authored

    CHECK_FALSE(session.set_plugin_env_stage(node_id, "ghost", 0, 0.5, 0.1));
    CHECK_FALSE(session.set_plugin_env_stage("no-such-node", "env", 0, 0.5, 0.1));
    std::filesystem::remove(path);
}

TEST_CASE("convolver refuses impulses beyond the 10 s memory guard", "[ntp]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.convbig", "name": "TOO BIG", "type": "fx",
      "graph": {
        "nodes": [{"id": "c", "type": "convolver", "impulse": "ir.wav"}],
        "connections": [
          {"from": "input", "to": "c"},
          {"from": "c", "to": "output"}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {"ir.wav", make_wav(500.0F, 10.5F, kRate)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    CHECK(plugin == nullptr);
    CHECK(errors_joined(errors).find("longer than 10 s") != std::string::npos);
}
