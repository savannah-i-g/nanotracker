// Stage 25 XINS verification: per-instance NTP state round-trips a full
// session save→load cycle. Envelope-stage edits persist as per-instance
// overrides (the shared manifest stays authored, and the reloaded
// instance's audible path carries the edited curve); native_stage ABI
// state chunks persist and restore through the reader. Both drive the
// same archive/PLGB/WPBR machinery the app uses, against a real
// dlopen'd stage binary.
#include "app/project_session.h"
#include "plugins/ntp_loader.h"
#include "plugins/ntp_stage_host.h"
#include "plugins/ntp_voices.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

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

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string stage_entry_name(const char* stage_name) {
    return nt::plugins::native_stage_archive_path(stage_name,
                                                  nt::plugins::native_stage_platform_tag());
}

std::filesystem::path write_temp(const char* name, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/file seam
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

float state_gain(const std::vector<std::uint8_t>& chunk) {
    REQUIRE(chunk.size() == sizeof(float));
    float value = 0.0F;
    std::memcpy(&value, chunk.data(), sizeof(float));
    return value;
}

// Instrument with a two-stage envelope driving the amp gain — the
// envelope editor's target.
std::vector<std::uint8_t> make_env_plugin() {
    const std::string manifest = R"({
      "ntp": 1, "id": "test.envstate", "name": "ENV STATE", "type": "instrument",
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
    return make_zip({{"plugin.json", to_bytes(manifest)}});
}

// FX plugin wrapping the gain native stage; its state is the gain of
// its last process() call (tests/fixtures/test_stage_gain.c).
std::vector<std::uint8_t> make_stage_plugin() {
    const std::string manifest = R"({
      "ntp": 1, "id": "test.stagestate", "name": "STAGE STATE", "type": "fx",
      "params": [{"key": "drv.gain", "label": "GAIN", "min": 0, "max": 2, "default": 1.5}],
      "graph": {
        "nodes": [{"id": "drv", "type": "native_stage", "stage": "gain"}],
        "connections": [{"from": "input", "to": "drv"}, {"from": "drv", "to": "output"}]
      }
    })";
    return make_zip({
        {"plugin.json", to_bytes(manifest)},
        {stage_entry_name("gain"), read_file(NT_TEST_STAGE_GAIN)},
    });
}

} // namespace

TEST_CASE("envelope-stage edits persist per-instance through XINS", "[ftrk][xins]") {
    const auto plugin_path = write_temp("nt_xins_env.ntins", make_env_plugin());
    const auto project_path = std::filesystem::temp_directory_path() / "nt_xins_env.ftrk";
    std::string workspace_id;

    {
        nt::audio::AudioEngine audio; // device-independent (never started)
        nt::app::ProjectSession session(audio);
        REQUIRE(session.load_plugin_file(plugin_path) == "test.envstate");
        workspace_id = session.add_plugin_node("test.envstate");
        REQUIRE_FALSE(workspace_id.empty());
        // Both stages down to a distinctive 0.25 — the envelope tops out
        // there, so the sustained amp gain is 0.25 (no overshoot).
        REQUIRE(session.set_plugin_env_stage(workspace_id, "env", 0, 0.25, 0.002));
        REQUIRE(session.set_plugin_env_stage(workspace_id, "env", 1, 0.25, 0.02));
        REQUIRE(session.save_ftrk(project_path));
    }

    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_ftrk(project_path));
    nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
    REQUIRE(instance != nullptr);

    // Override present on the reloaded instance; the shared manifest is
    // back at its authored curve (per-instance, not plugin-wide).
    const std::vector<ntp::EnvelopeStage> eff = instance->effective_env_stages("env");
    REQUIRE(eff.size() == 2);
    CHECK(eff[0].target == 0.25);
    CHECK(eff[1].target == 0.25);
    CHECK(instance->manifest().graph.nodes[1].env_stages[0].target == 1.0); // authored

    // Audible path: the voice runtimes carry the edited curve, so the
    // sustained output peaks at 0.5 (osc gain) * 0.25 (env) = 0.125,
    // well below the authored 0.6-sustain level (~0.3).
    instance->note_on(69, 1.0F);
    std::array<float, 256> block{};
    float peak = 0.0F;
    for (int b = 0; b < 48; ++b) { // ~0.128 s, past the ~0.022 s attack
        instance->process(nullptr, block.data(), 128);
        for (const float v : block) {
            peak = std::max(peak, std::abs(v));
        }
    }
    CHECK(peak > 0.07F);
    CHECK(peak < 0.20F);

    std::filesystem::remove(plugin_path);
    std::filesystem::remove(project_path);
}

TEST_CASE("native_stage state chunks persist through XINS", "[ftrk][xins]") {
    const auto plugin_path = write_temp("nt_xins_stage.ntins", make_stage_plugin());
    const auto project_path = std::filesystem::temp_directory_path() / "nt_xins_stage.ftrk";
    std::string workspace_id;

    {
        nt::audio::AudioEngine audio;
        nt::app::ProjectSession session(audio);
        REQUIRE(session.load_plugin_file(plugin_path) == "test.stagestate");
        workspace_id = session.add_plugin_node("test.stagestate");
        REQUIRE_FALSE(workspace_id.empty());
        nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
        REQUIRE(instance != nullptr);
        // The fixture stage's state is the gain of its last process()
        // call: drive it to a distinctive 0.75 (off the 1.5 default).
        instance->set_param("drv.gain", 0.75F);
        std::array<float, 256> in{};
        std::array<float, 256> out{};
        in.fill(0.1F);
        instance->process(in.data(), out.data(), 128);
        CHECK(state_gain(instance->native_stage_state("drv")) == 0.75F);
        REQUIRE(session.save_ftrk(project_path));
    }

    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_ftrk(project_path));
    nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
    REQUIRE(instance != nullptr);
    // The chunk restored through set_native_stage_state on load.
    CHECK(state_gain(instance->native_stage_state("drv")) == 0.75F);

    std::filesystem::remove(plugin_path);
    std::filesystem::remove(project_path);
}
