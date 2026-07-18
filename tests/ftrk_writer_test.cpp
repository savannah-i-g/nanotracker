// FTRK v14 writer verification: a project exercising every block
// (cells with bound slots, samples with payloads, FX strips +
// automation, instrument table + BNDT, sequence layers + channel
// colors, WPBR workspace JSON, PLGB archives, PPRS presets, POVR
// passthrough, XPLG external state) writes and reads back equal.
#include "engine/tracker_engine.h"
#include "io/ftrk_reader.h"
#include "io/ftrk_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("ftrk v14 round-trips every block", "[ftrk]") {
    nt::engine::TrackerProject project = nt::engine::create_project(4);
    project.name = "ROUNDTRIP";
    project.bpm = 140;
    project.speed = 5;

    // Pattern cells incl. the packed bound sub-slot.
    auto& cell = project.patterns[0].rows[3][2];
    cell.note = 49;
    cell.instrument = 7;
    cell.bound_index = 3;
    cell.volume = 0x30;
    cell.effect = 0xC;
    cell.effect_param = 0x20;
    project.patterns[0].name = "INTRO";
    project.patterns[0].highlight_major = 8;
    project.patterns[0].highlight_minor = 2;

    // A sample with payload bytes + metadata.
    nt::engine::TrackerSample sample;
    sample.id = 1;
    sample.name = "BLIP";
    sample.file_name = "blip.wav";
    sample.format = "wav";
    sample.original_data = {1, 2, 3, 4, 5, 6, 7, 8};
    sample.sample_rate = 44100;
    sample.num_channels = 1;
    sample.frames = 4;
    sample.loop_start = 1;
    sample.loop_length = 2;
    sample.base_note = 62;
    sample.finetune = -12;
    sample.volume = 48;
    sample.pan = 200;
    sample.stretch_ratio = 1.25;
    sample.category = 3;
    project.samples.push_back(sample);

    // FX mixer strip + one automation cell.
    nt::engine::FxChannelStrip strip;
    strip.name = "FX 01";
    strip.volume = 80;
    strip.pan = -25;
    strip.color = 0xFF3366;
    strip.enabled = true;
    strip.tracker_sends = {1.0F, 0.5F, 0.0F, 0.25F};
    nt::engine::FxModuleInstance module;
    module.module_id = "delay";
    module.params = {{"wet", 40.0F}, {"time", 0.25F}};
    strip.modules.push_back(module);
    project.fx_mixer.channels.push_back(strip);
    nt::engine::FxPattern fx;
    fx.present.assign(64, false);
    fx.cells.resize(64);
    fx.present[10] = true;
    fx.cells[10] = {.channel_index = 0, .module_index = 0, .param_key = "wet", .value = 75.0};
    project.fx_mixer.fx_patterns.push_back(std::move(fx));

    // Instrument table with all three types + bound tracks.
    nt::engine::InstrumentTableEntry sample_entry;
    sample_entry.type = nt::engine::InstrumentSourceType::kSample;
    sample_entry.sample_id = 1;
    sample_entry.bound_tracks = {0, 2};
    nt::engine::InstrumentTableEntry plugin_entry;
    plugin_entry.type = nt::engine::InstrumentSourceType::kPlugin;
    plugin_entry.plugin_id = "demo.pulsar";
    plugin_entry.plugin_preset_params = {{"osc.gain", 0.7}};
    nt::engine::InstrumentTableEntry workspace_entry;
    workspace_entry.type = nt::engine::InstrumentSourceType::kWorkspace;
    workspace_entry.workspace_id = "plg-1";
    project.instrument_table = {sample_entry, plugin_entry, workspace_entry};

    // Sequence layer + a custom channel color.
    nt::engine::SequencePattern seq;
    seq.layers.resize(4);
    nt::engine::SequenceLayer layer;
    layer.instrument = 2;
    layer.enabled = true;
    layer.notes = {{.pitch = 72, .start_tick = 12, .duration_ticks = 6, .velocity = 90}};
    seq.layers[1].push_back(layer);
    project.sequence_mixer.seq_patterns.push_back(std::move(seq));
    project.channel_colors[2] = 0xFF00CCAA;

    nt::io::FtrkWriteExtras extras;
    extras.workspace_json = R"({"instruments":[],"cables":[]})";
    extras.pprs_json = R"([{"instanceId":"plg-1","projectPresets":[]}])";
    extras.plugins.push_back({.plugin_id = "demo.pulsar", .archive = {9, 9, 9}});
    nt::io::FtrkExternalPlugin external;
    external.workspace_id = "clap-1";
    external.kind = 0;
    external.plugin_id = "nt.test.sine";
    external.library_path = "/tmp/test.clap";
    external.state = {0xAA, 0xBB, 0xCC};
    external.params = {{1, 0.5}, {7, 0.25}};
    extras.external.push_back(external);

    const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, extras);
    std::string error;
    auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
    INFO(error);
    REQUIRE(result.has_value());

    const nt::engine::TrackerProject& loaded = result->project;
    CHECK(result->extras.version == 14);
    CHECK(loaded.name == "ROUNDTRIP");
    CHECK(loaded.bpm == 140);
    CHECK(loaded.speed == 5);
    CHECK(loaded.channels == 4);
    REQUIRE(loaded.patterns.size() == project.patterns.size());
    CHECK(loaded.patterns[0].name == "INTRO");
    CHECK(loaded.patterns[0].highlight_major == 8);
    const auto& loaded_cell = loaded.patterns[0].rows[3][2];
    CHECK(loaded_cell.note == 49);
    CHECK(loaded_cell.instrument == 7);
    CHECK(loaded_cell.bound_index == 3);
    CHECK(loaded_cell.volume == 0x30);
    CHECK(loaded_cell.effect == 0xC);
    CHECK(loaded_cell.effect_param == 0x20);

    REQUIRE(loaded.samples.size() == 1);
    const auto& loaded_sample = loaded.samples[0];
    CHECK(loaded_sample.name == "BLIP");
    CHECK(loaded_sample.original_data == sample.original_data);
    CHECK(loaded_sample.finetune == -12);
    CHECK(loaded_sample.loop_length == 2);
    CHECK(loaded_sample.stretch_ratio > 1.24);
    CHECK(loaded_sample.category == 3);

    REQUIRE(result->extras.fx_channels.size() == 1);
    const auto& loaded_strip = result->extras.fx_channels[0];
    CHECK(loaded_strip.name == "FX 01");
    CHECK(loaded_strip.pan == -25);
    CHECK(loaded_strip.color == "#ff3366");
    REQUIRE(loaded_strip.modules.size() == 1);
    CHECK(loaded_strip.modules[0].params.size() == 2);
    REQUIRE(loaded.fx_mixer.fx_patterns.size() >= 1);
    CHECK(loaded.fx_mixer.fx_patterns[0].present[10]);
    CHECK(loaded.fx_mixer.fx_patterns[0].cells[10].param_key == "wet");

    REQUIRE(loaded.instrument_table.size() == 3);
    CHECK(loaded.instrument_table[0].sample_id == 1);
    CHECK(loaded.instrument_table[0].bound_tracks == std::vector<int>({0, 2}));
    CHECK(loaded.instrument_table[1].plugin_id == "demo.pulsar");
    CHECK(loaded.instrument_table[2].workspace_id == "plg-1");

    REQUIRE(loaded.sequence_mixer.seq_patterns.size() == 1);
    const auto& loaded_layers = loaded.sequence_mixer.seq_patterns[0].layers;
    REQUIRE(loaded_layers.size() == 4);
    REQUIRE(loaded_layers[1].size() >= 1);
    CHECK(loaded_layers[1][0].instrument == 2);
    REQUIRE(loaded_layers[1][0].notes.size() == 1);
    CHECK(loaded_layers[1][0].notes[0].pitch == 72);
    CHECK(loaded_layers[1][0].notes[0].start_tick == 12);
    CHECK(loaded.channel_colors[2] == 0xFF00CCAA);

    CHECK(result->extras.workspace_json == extras.workspace_json);
    CHECK(result->extras.pprs_json == extras.pprs_json);
    REQUIRE(result->extras.plugins.size() == 1);
    CHECK(result->extras.plugins[0].archive == std::vector<std::uint8_t>({9, 9, 9}));
    REQUIRE(result->extras.external.size() == 1);
    CHECK(result->extras.external[0].plugin_id == "nt.test.sine");
    CHECK(result->extras.external[0].state == std::vector<std::uint8_t>({0xAA, 0xBB, 0xCC}));
    REQUIRE(result->extras.external[0].params.size() == 2);
    CHECK(result->extras.external[0].params[1].second == 0.25);
}
