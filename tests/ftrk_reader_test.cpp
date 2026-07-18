// FTRK reader verification: fixture_v13.ftrk was produced by the WEB
// serializer (tools trace-dump/gen_ftrk against
// Source/.../src/lib/trackerSerializer.ts); fixture_v13.json is the
// same project as data. The reader must reproduce the manifest, fail
// loudly on core corruption, and skip damaged optional blocks.
#include "io/ftrk_reader.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <vector>

namespace {

using nlohmann::json;

json load_manifest() {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/fixture_v13.json");
    REQUIRE(file.good());
    return json::parse(file);
}

std::vector<std::uint8_t> load_fixture_bytes() {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/fixture_v13.ftrk", std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("FTRK v13 fixture loads faithfully", "[ftrk]") {
    std::string error;
    const auto result =
        nt::io::read_ftrk_file(std::string(NT_GOLDEN_DIR) + "/fixture_v13.ftrk", error);
    INFO(error);
    REQUIRE(result.has_value());
    const auto& [project, extras] = *result;
    const json m = load_manifest();

    CHECK(extras.version == 13);
    CHECK(extras.warnings.empty());

    // Header.
    CHECK(project.name == m.at("name").get<std::string>());
    CHECK(project.bpm == 140);
    CHECK(project.speed == m.at("speed"));
    CHECK(project.rows_per_pattern == m.at("rowsPerPattern"));
    CHECK(project.channels == m.at("channels"));
    CHECK(project.order_list == m.at("orderList").get<std::vector<int>>());

    // Patterns and cells (incl. v9 bound-index bit split, v6 per-pattern rows).
    const json& jpats = m.at("patterns");
    REQUIRE(project.patterns.size() == jpats.size());
    for (std::size_t pi = 0; pi < jpats.size(); ++pi) {
        const auto& pattern = project.patterns[pi];
        const json& jp = jpats[pi];
        CHECK(pattern.name == jp.at("name").get<std::string>());
        REQUIRE(pattern.rows.size() == jp.at("rows").size());
        for (std::size_t r = 0; r < pattern.rows.size(); ++r) {
            for (std::size_t ch = 0; ch < pattern.rows[r].size(); ++ch) {
                const json& jc = jp.at("rows")[r][ch];
                const auto& cell = pattern.rows[r][ch];
                CHECK(cell.note == jc.value("note", 0));
                CHECK(cell.instrument == jc.value("instrument", 0));
                CHECK(cell.volume == jc.value("volume", 0xFF));
                CHECK(cell.effect == jc.value("effect", 0));
                CHECK(cell.effect_param == jc.value("effectParam", 0));
                CHECK(cell.bound_index == jc.value("boundIndex", 0));
            }
        }
    }

    // Samples: metadata + byte-exact payload.
    const json& jsamples = m.at("samples");
    REQUIRE(project.samples.size() == jsamples.size());
    for (std::size_t i = 0; i < jsamples.size(); ++i) {
        const auto& s = project.samples[i];
        const json& js = jsamples[i];
        CHECK(s.id == js.at("id"));
        CHECK(s.name == js.at("name").get<std::string>());
        CHECK(s.file_name == js.at("fileName").get<std::string>());
        CHECK(s.format == js.at("format").get<std::string>());
        CHECK(s.sample_rate == js.at("sampleRate"));
        CHECK(s.num_channels == js.at("numChannels"));
        CHECK(s.frames == js.at("frames"));
        CHECK(s.loop_start == js.at("loopStart"));
        CHECK(s.loop_length == js.at("loopLength"));
        CHECK(s.base_note == js.at("baseNote"));
        CHECK(s.finetune == js.at("finetune"));
        CHECK(s.volume == js.at("volume"));
        CHECK(s.pan == js.at("pan"));
        CHECK(s.category == js.at("category"));
        CHECK(s.stretch_ratio == js.at("stretchRatio").get<double>());
        CHECK(s.original_data == js.at("originalData").get<std::vector<std::uint8_t>>());
    }

    // Instrument table + BNDT bound tracks.
    const json& jtable = m.at("instrumentTable");
    REQUIRE(project.instrument_table.size() == jtable.size());
    CHECK(project.instrument_table[0].type == nt::engine::InstrumentSourceType::kSample);
    CHECK(project.instrument_table[0].sample_id == 1);
    CHECK(project.instrument_table[0].bound_tracks == std::vector<int>{0, 2});
    CHECK(project.instrument_table[1].type == nt::engine::InstrumentSourceType::kPlugin);
    CHECK(project.instrument_table[1].plugin_id == "plugin:Bass@2");
    REQUIRE(project.instrument_table[1].plugin_preset_params.size() == 2);
    CHECK(project.instrument_table[2].type == nt::engine::InstrumentSourceType::kWorkspace);
    CHECK(project.instrument_table[2].workspace_id == "ws-fixture-1");

    // Sequence layers + sparse channel colors.
    REQUIRE(project.sequence_mixer.seq_patterns.size() == 2);
    const auto& sp0 = project.sequence_mixer.seq_patterns[0];
    REQUIRE(sp0.layers.size() == 3);
    REQUIRE(sp0.layers[0][0].notes.size() == 2);
    CHECK(sp0.layers[0][0].notes[1].pitch == 64);
    CHECK(sp0.layers[0][0].notes[1].start_tick == 8);
    CHECK_FALSE(sp0.layers[1][0].enabled);
    CHECK(project.channel_colors[0] == 0xFFFF0055U);
    CHECK(project.channel_colors[1] == 0U);
    CHECK(project.channel_colors[2] == 0xFF00FF88U);

    // FX mixer: channel strips + automation patterns.
    REQUIRE(extras.fx_channels.size() == 2);
    CHECK(extras.fx_channels[0].name == "FX 01");
    CHECK(extras.fx_channels[0].pan == -25);
    REQUIRE(extras.fx_channels[0].modules.size() == 2);
    CHECK(extras.fx_channels[0].modules[0].module_id == "delay");
    CHECK(extras.fx_channels[0].tracker_sends == std::vector<float>{0.5F, 0.0F, 1.0F});
    REQUIRE(project.fx_mixer.fx_patterns.size() == 2);
    CHECK(project.fx_mixer.fx_patterns[0].present[2]);
    CHECK(project.fx_mixer.fx_patterns[0].cells[2].param_key == "cutoff");
    CHECK_FALSE(project.fx_mixer.fx_patterns[0].present[3]);

    // Workspace JSON, bundled plugins, PPRS.
    const json workspace = json::parse(extras.workspace_json);
    REQUIRE(workspace.at("instruments").size() == 1);
    CHECK(workspace.at("instruments")[0].at("workspaceId") == "ws-fixture-1");
    CHECK(workspace.at("cables")[0].at("mode") == "tap");
    REQUIRE(extras.plugins.size() == 1);
    CHECK(extras.plugins[0].plugin_id == "plugin:Bass@2");
    CHECK(extras.plugins[0].archive.size() == 128);
    const json pprs = json::parse(extras.pprs_json);
    CHECK(pprs[0].at("activePresetId") == "preset-1");
}

TEST_CASE("FTRK reader fails loudly on core corruption", "[ftrk]") {
    std::string error;

    SECTION("bad magic") {
        std::vector<std::uint8_t> bytes = load_fixture_bytes();
        bytes[0] = 'X';
        CHECK_FALSE(nt::io::read_ftrk(bytes.data(), bytes.size(), error).has_value());
        CHECK(error.find("magic") != std::string::npos);
    }

    SECTION("unsupported version") {
        std::vector<std::uint8_t> bytes = load_fixture_bytes();
        bytes[4] = 99;
        CHECK_FALSE(nt::io::read_ftrk(bytes.data(), bytes.size(), error).has_value());
        CHECK(error.find("version") != std::string::npos);
    }

    SECTION("truncated core") {
        const std::vector<std::uint8_t> bytes = load_fixture_bytes();
        CHECK_FALSE(nt::io::read_ftrk(bytes.data(), 200, error).has_value());
        CHECK(error.find("truncated") != std::string::npos);
    }
}

TEST_CASE("FTRK reader skips damaged optional blocks", "[ftrk]") {
    // Corrupt the FXMX magic (block becomes unreadable); the song core
    // must still load, with a warning recorded.
    std::vector<std::uint8_t> bytes = load_fixture_bytes();
    // fxBlockOffset lives at header offset 49 (4+2+32+4+1+1+1+1+2+1).
    std::uint32_t fx_offset = 0;
    std::memcpy(&fx_offset, bytes.data() + 49, 4);
    REQUIRE(fx_offset != 0);
    bytes[fx_offset] = 'X';

    std::string error;
    const auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->extras.warnings.empty());
    CHECK(result->project.name == "FIXTURE V13");
    CHECK(result->extras.fx_channels.empty());
}
