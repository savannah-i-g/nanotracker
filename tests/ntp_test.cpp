// NTP v1 verification: manifest validation (strict, collected errors),
// archive loading through a real in-memory ZIP, and deterministic
// instance processing — a playable synth voice (oscillator + envelope
// via audio-rate param connection + pitch mod route), an FX delay
// graph, and sampler zone/round-robin behaviour.
#include "plugins/ntp_loader.h"
#include "plugins/ntp_voices.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
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
      "requires": ["userSamples"]
    })";
    CHECK_FALSE(nt::plugins::parse_ntp_manifest(bad, manifest, errors));
    const std::string all = errors_joined(errors);
    CHECK(all.find("missing or unsupported \"ntp\"") != std::string::npos);
    CHECK(all.find("missing plugin name") != std::string::npos);
    CHECK(all.find("min must be < max") != std::string::npos);
    CHECK(all.find("default outside range") != std::string::npos);
    CHECK(all.find("duplicate parameter key") != std::string::npos);
    CHECK(all.find("worklet") != std::string::npos);
    CHECK(all.find("native_stage is reserved") != std::string::npos);
    CHECK(all.find("unknown type \"mystery\"") != std::string::npos);
    CHECK(all.find("unknown endpoint \"ghost\"") != std::string::npos);
    CHECK(all.find("unknown capability requirement") != std::string::npos);
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
