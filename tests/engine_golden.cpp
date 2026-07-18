// Golden-vector verification: the C++ engine must reproduce, tick for
// tick, the traces dumped from the web engine (the behavioural
// reference implementation). Fixtures live in tests/golden/*.json as
// {project, ticks[]}; the dumper is tools/trace-dump (run against
// Source/.../src/lib/trackerEngine.ts).
#include "engine/tracker_engine.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

namespace {

using nlohmann::json;
using namespace nt::engine;

InstrumentSourceType type_from_string(const std::string& s) {
    if (s == "plugin") {
        return InstrumentSourceType::kPlugin;
    }
    if (s == "workspace") {
        return InstrumentSourceType::kWorkspace;
    }
    return InstrumentSourceType::kSample;
}

TrackerProject project_from_json(const json& j) {
    TrackerProject p;
    p.name = j.value("name", "UNTITLED");
    p.bpm = j.value("bpm", 125);
    p.speed = j.value("speed", 6);
    p.rows_per_pattern = j.value("rowsPerPattern", 64);
    p.channels = j.value("channels", 4);
    p.order_list = j.value("orderList", std::vector<int>{});

    for (const json& jp : j.value("patterns", json::array())) {
        TrackerPattern pattern;
        pattern.id = jp.value("id", 0);
        pattern.name = jp.value("name", "");
        for (const json& jrow : jp.value("rows", json::array())) {
            std::vector<TrackerCell> row;
            for (const json& jc : jrow) {
                TrackerCell cell;
                cell.note = static_cast<std::uint8_t>(jc.value("note", 0));
                cell.instrument = static_cast<std::uint8_t>(jc.value("instrument", 0));
                cell.volume = static_cast<std::uint8_t>(jc.value("volume", 0xFF));
                cell.effect = static_cast<std::uint8_t>(jc.value("effect", 0));
                cell.effect_param = static_cast<std::uint8_t>(jc.value("effectParam", 0));
                cell.bound_index = static_cast<std::uint8_t>(jc.value("boundIndex", 0));
                row.push_back(cell);
            }
            pattern.rows.push_back(std::move(row));
        }
        p.patterns.push_back(std::move(pattern));
    }

    for (const json& js : j.value("samples", json::array())) {
        TrackerSample s;
        s.id = js.value("id", 0);
        s.volume = js.value("volume", 64);
        s.pan = js.value("pan", 128);
        s.base_note = js.value("baseNote", 60);
        s.finetune = js.value("finetune", 0);
        p.samples.push_back(std::move(s));
    }

    for (const json& je : j.value("instrumentTable", json::array())) {
        InstrumentTableEntry e;
        e.type = type_from_string(je.value("type", "sample"));
        e.sample_id = je.value("sampleId", 0);
        e.plugin_id = je.value("pluginId", "");
        e.workspace_id = je.value("workspaceId", "");
        e.bound_tracks = je.value("boundTracks", std::vector<int>{});
        p.instrument_table.push_back(std::move(e));
    }

    if (j.contains("sequenceMixer") && j["sequenceMixer"].is_object()) {
        for (const json& jsp : j["sequenceMixer"].value("seqPatterns", json::array())) {
            SequencePattern sp;
            for (const json& jch : jsp.value("layers", json::array())) {
                std::vector<SequenceLayer> layers;
                for (const json& jl : jch) {
                    SequenceLayer layer;
                    layer.instrument = jl.value("instrument", 0);
                    layer.enabled = jl.value("enabled", true);
                    for (const json& jn : jl.value("notes", json::array())) {
                        layer.notes.push_back({.pitch = jn.value("pitch", 60),
                                               .start_tick = jn.value("startTick", 0),
                                               .duration_ticks = jn.value("durationTicks", 1),
                                               .velocity = jn.value("velocity", 100)});
                    }
                    layers.push_back(std::move(layer));
                }
                sp.layers.push_back(std::move(layers));
            }
            p.sequence_mixer.seq_patterns.push_back(std::move(sp));
        }
    }

    if (j.contains("fxMixer") && j["fxMixer"].is_object()) {
        for (const json& jfp : j["fxMixer"].value("fxPatterns", json::array())) {
            FxPattern fx;
            for (const json& jr : jfp.value("rows", json::array())) {
                if (jr.is_null()) {
                    fx.present.push_back(false);
                    fx.cells.emplace_back();
                } else {
                    fx.present.push_back(true);
                    FxCell cell;
                    cell.channel_index = jr.value("channelIndex", 0);
                    cell.param_key = jr.value("paramKey", "");
                    cell.value = jr.value("value", 0.0);
                    fx.cells.push_back(std::move(cell));
                }
            }
            p.fx_mixer.fx_patterns.push_back(std::move(fx));
        }
    }

    return p;
}

// Compares one advanced state against a dumped web tick. Returns a
// description of the first mismatch, empty when equal.
std::string compare_tick(const TrackerPlayState& s, const json& t, int channel_count) {
#define NT_CHECK(field, expr)                                                                      \
    if ((expr) != t.value(field, decltype(expr){})) {                                              \
        return std::string(field) + ": got " + std::to_string(expr);                               \
    }
    NT_CHECK("orderPos", s.order_pos)
    NT_CHECK("patternIndex", s.pattern_index)
    NT_CHECK("row", s.row)
    NT_CHECK("tick", s.tick)
    NT_CHECK("bpm", s.bpm)
    NT_CHECK("speed", s.speed)
    NT_CHECK("patternEnded", s.pattern_ended)
    NT_CHECK("songEnded", s.song_ended)
    NT_CHECK("loopRow", s.loop_row)
    NT_CHECK("loopCount", s.loop_count)
    NT_CHECK("loopActive", s.loop_active)
    NT_CHECK("hasLoopJump", s.has_loop_jump)
    NT_CHECK("patternLoopFlush", s.pattern_loop_flush)
    NT_CHECK("patternDelay", s.pattern_delay)
    NT_CHECK("nextOrderPos", s.next_order_pos)
    NT_CHECK("nextRow", s.next_row)
    NT_CHECK("hasJump", s.has_jump)
#undef NT_CHECK

    // FX automation cell.
    const json& jfx = t.at("fxCell");
    if (jfx.is_null() != (s.fx_automation_cell == nullptr)) {
        return "fxCell presence mismatch";
    }
    if (!jfx.is_null()) {
        if (s.fx_automation_cell->channel_index != jfx.value("channelIndex", -1) ||
            s.fx_automation_cell->param_key != jfx.value("paramKey", "") ||
            s.fx_automation_cell->value != jfx.value("value", 0.0)) {
            return "fxCell content mismatch";
        }
    }

    // Sequence triggers (order matters).
    const json& jtrigs = t.at("seqTriggers");
    if (static_cast<int>(jtrigs.size()) != s.seq_trigger_count) {
        return "seqTriggers count: got " + std::to_string(s.seq_trigger_count) + " want " +
               std::to_string(jtrigs.size());
    }
    for (int i = 0; i < s.seq_trigger_count; ++i) {
        const json& jt = jtrigs[static_cast<std::size_t>(i)];
        const SequenceTrigger& st = s.seq_triggers[static_cast<std::size_t>(i)];
        if (st.channel_index != jt.value("channelIndex", -1) ||
            st.layer_index != jt.value("layerIndex", -1) || st.pitch != jt.value("pitch", -1) ||
            st.velocity != jt.value("velocity", -1) ||
            st.is_note_on != jt.value("isNoteOn", false) ||
            st.instrument != jt.value("instrument", -1)) {
            return "seqTrigger " + std::to_string(i) + " mismatch";
        }
    }

    // Channels.
    const json& jchs = t.at("channels");
    for (int ch = 0; ch < channel_count; ++ch) {
        const json& jc = jchs[static_cast<std::size_t>(ch)];
        const ChannelPlayState& c = s.channels[static_cast<std::size_t>(ch)];
        const std::string where = "ch" + std::to_string(ch) + ".";
#define NT_CHECKC(field, expr)                                                                     \
    if ((expr) != jc.value(field, decltype(expr){})) {                                             \
        return where + field + ": got " + std::to_string(expr) + " want " + jc.at(field).dump();   \
    }
        NT_CHECKC("note", c.note)
        NT_CHECKC("instrument", c.instrument)
        NT_CHECKC("volume", c.volume)
        NT_CHECKC("pan", c.pan)
        NT_CHECKC("period", c.period)
        NT_CHECKC("portaTarget", c.porta_target)
        NT_CHECKC("portaSpeed", c.porta_speed)
        NT_CHECKC("vibratoPhase", c.vibrato_phase)
        NT_CHECKC("vibratoSpeed", c.vibrato_speed)
        NT_CHECKC("vibratoDepth", c.vibrato_depth)
        NT_CHECKC("tremoloPhase", c.tremolo_phase)
        NT_CHECKC("tremoloSpeed", c.tremolo_speed)
        NT_CHECKC("tremoloDepth", c.tremolo_depth)
        NT_CHECKC("sampleOffset", c.sample_offset)
        NT_CHECKC("retrigCount", c.retrig_count)
        NT_CHECKC("retrigParam", c.retrig_param)
        NT_CHECKC("arpeggio0", c.arpeggio0)
        NT_CHECKC("arpeggio1", c.arpeggio1)
        NT_CHECKC("arpeggio2", c.arpeggio2)
        NT_CHECKC("volSlide", c.vol_slide)
        NT_CHECKC("finetune", c.finetune)
        NT_CHECKC("triggerNote", c.trigger_note)
        NT_CHECKC("releaseNote", c.release_note)
        NT_CHECKC("periodOverride", c.period_override)
        NT_CHECKC("volumeOverride", c.volume_override)
        NT_CHECKC("rowEffect", c.row_effect)
        NT_CHECKC("rowEffectParam", c.row_effect_param)
#undef NT_CHECKC
        if (c.instrument_type != type_from_string(jc.value("instrumentType", "sample"))) {
            return where + "instrumentType mismatch";
        }
    }
    return {};
}

void run_fixture(const std::string& name) {
    std::ifstream file(std::string(NT_GOLDEN_DIR) + "/" + name + ".json");
    REQUIRE(file.good());
    const json fixture = json::parse(file);

    const TrackerProject project = project_from_json(fixture.at("project"));
    TrackerPlayState state = create_play_state(project);
    state.is_playing = true;

    const json& ticks = fixture.at("ticks");
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        advance_tick(state, project);
        const std::string mismatch =
            compare_tick(state, ticks[i], std::min(project.channels, kMaxChannels));
        INFO(name << " tick " << i << ": " << mismatch);
        REQUIRE(mismatch.empty());
    }
}

} // namespace

TEST_CASE("engine reproduces web golden traces", "[engine][golden]") {
    for (const char* fixture :
         {"basic-flow", "arp-vib-trem", "porta-family", "volume-pan", "flow-jumps", "loops-delays",
          "speed-bpm", "seq-layers", "instrument-table", "edge-misc"}) {
        SECTION(fixture) {
            run_fixture(fixture);
        }
    }
}
