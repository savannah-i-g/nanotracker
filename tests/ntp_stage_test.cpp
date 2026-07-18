// Native-stage C ABI verification against real dlopen'd stage
// binaries (tests/fixtures/test_stage_gain.c and
// test_stage_wrong_version.c, built in-tree like the CLAP fixture):
// archive loading through the binaries/<platform-tag>/ layout, exact
// DSP through the NodeRuntime trampoline under the RT allocation
// guard, parameter delivery through the ParamSlot path (host params,
// clamping, audio-rate toParam collapse), state round-trips, reset,
// and the strict refusal paths — wrong ABI version naming both
// versions, missing platform binary listing the tags present.
#include "plugins/ntp_loader.h"
#include "plugins/ntp_stage_host.h"
#include "plugins/ntp_voices.h"
#include "rt/rt_assert.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
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

// The fixture binary as built for this platform (CMake passes the
// target path); the zip entry renames it into the archive layout.
std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string stage_entry_name(const char* stage_name) {
    return nt::plugins::native_stage_archive_path(stage_name,
                                                  nt::plugins::native_stage_platform_tag());
}

// One 128-frame block through the FX path under the RT allocation
// guard, all input samples = `input`; returns the interleaved output.
std::array<float, 256> process_block(nt::plugins::NtpInstance& instance, float input) {
    std::array<float, 256> in{};
    std::array<float, 256> out{};
    in.fill(input);
    {
        [[maybe_unused]] const nt::rt::RtScope rt_scope;
        instance.process(in.data(), out.data(), 128);
    }
    return out;
}

float state_gain(const std::vector<std::uint8_t>& chunk) {
    REQUIRE(chunk.size() == sizeof(float));
    float value = 0.0F;
    std::memcpy(&value, chunk.data(), sizeof(float));
    return value;
}

// FX manifest around one gain-stage node; host params bind the
// stage's descriptor parameters through their dot-path keys.
const std::string kStageFxManifest = R"({
  "ntp": 1, "id": "test.stage", "name": "STAGE FX", "type": "fx",
  "params": [
    {"key": "drv.gain", "label": "GAIN", "min": 0, "max": 2, "default": 1.5},
    {"key": "drv.offset", "label": "OFFSET", "min": -1, "max": 1, "default": 0.25}
  ],
  "graph": {
    "nodes": [{"id": "drv", "type": "native_stage", "stage": "gain"}],
    "connections": [
      {"from": "input", "to": "drv"},
      {"from": "drv", "to": "output"}
    ]
  }
})";

} // namespace

TEST_CASE("native_stage manifest validation is strict and collected", "[ntp_stage]") {
    const std::string bad = R"({
      "ntp": 1, "id": "test.badstage", "name": "BAD", "type": "instrument",
      "graph": {
        "nodes": [
          {"id": "a", "type": "native_stage"},
          {"id": "b", "type": "native_stage", "stage": "sub/dir"},
          {"id": "c", "type": "native_stage", "stage": "ok", "scope": "voice"}
        ]
      }
    })";
    ntp::Manifest manifest;
    std::vector<std::string> errors;
    CHECK_FALSE(nt::plugins::parse_ntp_manifest(bad, manifest, errors));
    const std::string all = errors_joined(errors);
    CHECK(all.find("native_stage needs a stage") != std::string::npos);
    CHECK(all.find("must be a bare basename") != std::string::npos);
    CHECK(all.find("native_stage is instance-scoped") != std::string::npos);
}

TEST_CASE("native stage loads, is marked, and processes exactly", "[ntp_stage]") {
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(kStageFxManifest)},
        {stage_entry_name("gain"), read_file(NT_TEST_STAGE_GAIN)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);
    // The trust flag the UI markers key off — never silent.
    CHECK(plugin->executes_native_code);
    REQUIRE(plugin->stages.contains("gain"));

    nt::plugins::NtpInstance instance(*plugin, kRate);

    // Host param defaults reached the stage: 0.5 * 1.5 + 0.25 = 1.0,
    // exact in float — the whole block must match bit-for-bit.
    std::array<float, 256> out = process_block(instance, 0.5F);
    for (const float v : out) {
        REQUIRE(v == 1.0F);
    }

    // Param sweep through the ParamSlot path (UI set_param → base).
    instance.set_param("drv.gain", 0.5F);
    out = process_block(instance, 0.5F);
    CHECK(out[0] == 0.5F);
    CHECK(out[255] == 0.5F);

    // Out-of-range writes clamp to the descriptor range (gain max 2).
    instance.set_param("drv.gain", 5.0F);
    out = process_block(instance, 0.5F);
    CHECK(out[0] == 1.25F);
    CHECK(out[255] == 1.25F);
}

TEST_CASE("audio-rate param connections collapse to block rate for stages", "[ntp_stage]") {
    // constant(0.5) → drv.offset via toParam: a native stage parameter
    // is k-rate by definition, so the connection contributes its
    // frame-0 sample. gain stays at the descriptor default (1).
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.stagemod", "name": "STAGE MOD", "type": "fx",
      "graph": {
        "nodes": [
          {"id": "c", "type": "constant", "value": 0.5},
          {"id": "drv", "type": "native_stage", "stage": "gain"}
        ],
        "connections": [
          {"from": "input", "to": "drv"},
          {"from": "c", "to": "drv", "toParam": "offset"},
          {"from": "drv", "to": "output"}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {stage_entry_name("gain"), read_file(NT_TEST_STAGE_GAIN)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);
    const std::array<float, 256> out = process_block(instance, 0.25F);
    // 0.25 * 1.0 + 0.5 = 0.75, exact.
    CHECK(out[0] == 0.75F);
    CHECK(out[255] == 0.75F);
}

TEST_CASE("native stage state round-trips and reset restores defaults", "[ntp_stage]") {
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(kStageFxManifest)},
        {stage_entry_name("gain"), read_file(NT_TEST_STAGE_GAIN)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    INFO(errors_joined(errors));
    REQUIRE(plugin != nullptr);

    nt::plugins::NtpInstance instance(*plugin, kRate);

    // The fixture's state is the gain of its last process() call.
    (void)process_block(instance, 0.5F); // manifest default gain 1.5
    const std::vector<std::uint8_t> saved = instance.native_stage_state("drv");
    CHECK(state_gain(saved) == 1.5F);

    instance.set_param("drv.gain", 0.75F);
    (void)process_block(instance, 0.5F);
    CHECK(state_gain(instance.native_stage_state("drv")) == 0.75F);

    // Load the earlier chunk → an immediate save returns it verbatim.
    REQUIRE(instance.set_native_stage_state("drv", saved));
    CHECK(instance.native_stage_state("drv") == saved);

    // Reset (the kSetBundle-drain path, audio thread) restores the
    // stage's post-create condition.
    {
        [[maybe_unused]] const nt::rt::RtScope rt_scope;
        instance.plugin_reset();
    }
    CHECK(state_gain(instance.native_stage_state("drv")) == 1.0F);

    // Unknown / non-stage node ids are refused, empty chunks too.
    CHECK(instance.native_stage_state("ghost").empty());
    CHECK_FALSE(instance.set_native_stage_state("ghost", saved));
    CHECK_FALSE(instance.set_native_stage_state("drv", {}));
}

TEST_CASE("wrong ABI version is refused naming both versions", "[ntp_stage]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.stagewrong", "name": "WRONG", "type": "fx",
      "graph": {
        "nodes": [{"id": "w", "type": "native_stage", "stage": "wrong"}],
        "connections": [
          {"from": "input", "to": "w"},
          {"from": "w", "to": "output"}
        ]
      }
    })";
    const std::vector<std::uint8_t> zip = make_zip({
        {"plugin.json", to_bytes(manifest_json)},
        {stage_entry_name("wrong"), read_file(NT_TEST_STAGE_WRONG_VERSION)},
    });
    std::vector<std::string> errors;
    auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
    CHECK(plugin == nullptr);
    const std::string all = errors_joined(errors);
    CHECK(all.find("ABI version 999") != std::string::npos);
    CHECK(all.find("implements ABI version 1") != std::string::npos);
}

TEST_CASE("missing platform binary refusal lists the tags present", "[ntp_stage]") {
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.stagemissing", "name": "MISSING", "type": "fx",
      "graph": {
        "nodes": [{"id": "drv", "type": "native_stage", "stage": "gain"}],
        "connections": [
          {"from": "input", "to": "drv"},
          {"from": "drv", "to": "output"}
        ]
      }
    })";
    const std::string host_tag = nt::plugins::native_stage_platform_tag();

    SECTION("other platforms shipped: the refusal names them") {
        const std::vector<std::uint8_t> zip = make_zip({
            {"plugin.json", to_bytes(manifest_json)},
            {"binaries/atari-st/gain.so", {0x00, 0x01, 0x02, 0x03}},
            {"binaries/amiga-68k/gain.dll", {0x00, 0x01, 0x02, 0x03}},
        });
        std::vector<std::string> errors;
        auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
        CHECK(plugin == nullptr);
        const std::string all = errors_joined(errors);
        CHECK(all.find("no binary for this platform (" + host_tag + ")") != std::string::npos);
        CHECK(all.find("atari-st") != std::string::npos);
        CHECK(all.find("amiga-68k") != std::string::npos);
    }

    SECTION("no binaries at all") {
        const std::vector<std::uint8_t> zip = make_zip({{"plugin.json", to_bytes(manifest_json)}});
        std::vector<std::string> errors;
        auto plugin = nt::plugins::load_ntp_archive(zip.data(), zip.size(), kRate, errors);
        CHECK(plugin == nullptr);
        CHECK(errors_joined(errors).find("archive provides none") != std::string::npos);
    }
}
