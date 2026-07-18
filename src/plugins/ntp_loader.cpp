#include "plugins/ntp_loader.h"

#include "audio/decoders.h"
#include "plugins/image_decode.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <miniz.h>
#include <optional>
#include <set>

namespace nt::plugins {

namespace {

using nlohmann::json;

// ── Enum maps ────────────────────────────────────────────────────────

std::optional<ntp::NodeType> node_type_from(const std::string& s) {
    if (s == "gain") {
        return ntp::NodeType::kGain;
    }
    if (s == "delay") {
        return ntp::NodeType::kDelay;
    }
    if (s == "biquad") {
        return ntp::NodeType::kBiquad;
    }
    if (s == "compressor") {
        return ntp::NodeType::kCompressor;
    }
    if (s == "convolver") {
        return ntp::NodeType::kConvolver;
    }
    if (s == "panner") {
        return ntp::NodeType::kPanner;
    }
    if (s == "waveshaper") {
        return ntp::NodeType::kWaveshaper;
    }
    if (s == "mixer") {
        return ntp::NodeType::kMixer;
    }
    if (s == "oscillator") {
        return ntp::NodeType::kOscillator;
    }
    if (s == "noise") {
        return ntp::NodeType::kNoise;
    }
    if (s == "constant") {
        return ntp::NodeType::kConstant;
    }
    if (s == "lfo") {
        return ntp::NodeType::kLfo;
    }
    if (s == "envelope") {
        return ntp::NodeType::kEnvelope;
    }
    if (s == "granular") {
        return ntp::NodeType::kGranular;
    }
    if (s == "wavetable") {
        return ntp::NodeType::kWavetable;
    }
    if (s == "sampler") {
        return ntp::NodeType::kSampler;
    }
    if (s == "native_stage") {
        return ntp::NodeType::kNativeStage;
    }
    return std::nullopt;
}

std::optional<ntp::ControlType> control_type_from(const std::string& s) {
    if (s == "knob") {
        return ntp::ControlType::kKnob;
    }
    if (s == "slider") {
        return ntp::ControlType::kSlider;
    }
    if (s == "toggle") {
        return ntp::ControlType::kToggle;
    }
    if (s == "select") {
        return ntp::ControlType::kSelect;
    }
    if (s == "number") {
        return ntp::ControlType::kNumber;
    }
    if (s == "xy_pad") {
        return ntp::ControlType::kXyPad;
    }
    if (s == "envelope_editor") {
        return ntp::ControlType::kEnvelopeEditor;
    }
    if (s == "meter") {
        return ntp::ControlType::kMeter;
    }
    if (s == "label") {
        return ntp::ControlType::kLabel;
    }
    if (s == "group") {
        return ntp::ControlType::kGroup;
    }
    if (s == "image") {
        return ntp::ControlType::kImage;
    }
    if (s == "sprite") {
        return ntp::ControlType::kSprite;
    }
    return std::nullopt;
}

ntp::ParamCurve curve_from(const std::string& s) {
    if (s == "exponential") {
        return ntp::ParamCurve::kExponential;
    }
    if (s == "logarithmic") {
        return ntp::ParamCurve::kLogarithmic;
    }
    return ntp::ParamCurve::kLinear;
}

std::optional<ntp::ModTransform> transform_from(const std::string& s) {
    if (s == "none") {
        return ntp::ModTransform::kNone;
    }
    if (s == "invert") {
        return ntp::ModTransform::kInvert;
    }
    if (s == "abs") {
        return ntp::ModTransform::kAbs;
    }
    if (s == "square") {
        return ntp::ModTransform::kSquare;
    }
    if (s == "unipolar") {
        return ntp::ModTransform::kUnipolar;
    }
    if (s == "bipolar") {
        return ntp::ModTransform::kBipolar;
    }
    return std::nullopt;
}

std::optional<ntp::PortKind> port_kind_from(const std::string& s) {
    if (s == "audio") {
        return ntp::PortKind::kAudio;
    }
    if (s == "sidechain") {
        return ntp::PortKind::kSidechain;
    }
    if (s == "cv") {
        return ntp::PortKind::kCv;
    }
    if (s == "gate") {
        return ntp::PortKind::kGate;
    }
    if (s == "midi") {
        return ntp::PortKind::kMidi;
    }
    return std::nullopt;
}

ntp::LoopMode loop_mode_from(const json& j) {
    if (j.is_boolean()) {
        // Converter output never emits booleans, but accepting the
        // web's v1 shorthand costs nothing and loses nothing.
        return j.get<bool>() ? ntp::LoopMode::kForward : ntp::LoopMode::kNone;
    }
    const std::string s = j.is_string() ? j.get<std::string>() : "none";
    if (s == "forward") {
        return ntp::LoopMode::kForward;
    }
    if (s == "pingpong") {
        return ntp::LoopMode::kPingpong;
    }
    if (s == "release") {
        return ntp::LoopMode::kRelease;
    }
    return ntp::LoopMode::kNone;
}

// ── Section parsers ──────────────────────────────────────────────────

// "grid:N" → N; anything else (including bare "grid") → nullopt.
std::optional<int> grid_slice_count(const std::string& auto_detect) {
    if (!auto_detect.starts_with("grid:")) {
        return std::nullopt;
    }
    const std::string count = auto_detect.substr(5);
    if (count.empty() || count.size() > 3 ||
        !std::all_of(count.begin(), count.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    return std::stoi(count);
}

ntp::SliceMap parse_slice_map(const json& j, const std::string& node_id,
                              std::vector<std::string>& errors) {
    ntp::SliceMap map;
    map.source = j.value("source", "");
    if (map.source.empty()) {
        errors.push_back("node \"" + node_id + "\": sliceMap needs a source");
    }
    const std::string mode = j.value("triggerMode", "oneShot");
    if (mode == "gated") {
        map.trigger_mode = ntp::SliceTriggerMode::kGated;
    } else if (mode != "oneShot") {
        errors.push_back("node \"" + node_id +
                         "\": sliceMap triggerMode must be "
                         "\"oneShot\" or \"gated\" (got \"" +
                         mode + "\")");
    }

    for (const json& sj : j.value("slices", json::array())) {
        ntp::SliceEntry slice;
        slice.start = sj.value("start", 0.0);
        slice.end = sj.value("end", 0.0);
        slice.note = sj.value("note", -1);
        slice.velocity_min = sj.value("velocity", 1);
        slice.choke_group = sj.value("choke", "");
        slice.round_robin_group = sj.value("roundRobinGroup", "");
        slice.release_on_gate = sj.value("releaseOnGate", true);
        const std::size_t index = map.slices.size();
        if (slice.start < 0.0 || slice.end <= slice.start) {
            errors.push_back("node \"" + node_id + "\": slice " + std::to_string(index) +
                             " needs 0 <= start < end");
        }
        if (slice.note > 127) {
            errors.push_back("node \"" + node_id + "\": slice " + std::to_string(index) +
                             " note outside 0-127");
        }
        if (slice.note < 0 && ntp::kSliceBaseNote + static_cast<int>(index) > 127) {
            errors.push_back("node \"" + node_id + "\": slice " + std::to_string(index) +
                             " has no note and the default 36+index exceeds 127");
        }
        if (slice.velocity_min < 1 || slice.velocity_min > 127) {
            errors.push_back("node \"" + node_id + "\": slice " + std::to_string(index) +
                             " velocity outside 1-127");
        }
        map.slices.push_back(std::move(slice));
    }

    // autoDetect and authored slices are exclusive: the loader either
    // takes the author's list verbatim or derives one.
    if (j.contains("autoDetect") && !j["autoDetect"].is_null()) {
        map.auto_detect = j["autoDetect"].is_string() ? j["autoDetect"].get<std::string>() : "";
        if (!map.slices.empty()) {
            errors.push_back("node \"" + node_id +
                             "\": sliceMap has both slices[] and autoDetect — author one");
        }
        if (map.auto_detect == "transients") {
            // The web's onset detector never shipped (v4.1.0 fell back
            // to grid:16); the native detector is parked the same way.
            // Normalised here so the runtime only ever sees grid.
            map.auto_detect = "grid:16";
        }
        if (map.auto_detect == "markers") {
            errors.push_back("node \"" + node_id +
                             "\": autoDetect \"markers\" (WAV cue chunks) is not in native v1 — "
                             "author explicit slices[]");
        } else {
            const std::optional<int> count = grid_slice_count(map.auto_detect);
            if (!count.has_value() || *count < 1 || ntp::kSliceBaseNote + *count - 1 > 127) {
                errors.push_back("node \"" + node_id +
                                 "\": autoDetect must be \"grid:N\" "
                                 "(N 1-92), got \"" +
                                 map.auto_detect + "\"");
            }
        }
    } else if (map.slices.empty()) {
        errors.push_back("node \"" + node_id + "\": sliceMap needs slices[] or autoDetect");
    }
    return map;
}

ntp::DspNode parse_node(const json& j, std::vector<std::string>& errors, bool is_instrument) {
    ntp::DspNode node;
    node.id = j.value("id", "");
    const std::string type_name = j.value("type", "");
    if (node.id.empty()) {
        errors.emplace_back("graph node without an id");
    }
    if (type_name == "worklet") {
        errors.push_back("node \"" + node.id +
                         "\": type \"worklet\" is a web-only escape hatch; run the manifest "
                         "through ntp-convert and replace the worklet with built-in nodes");
        return node;
    }
    const auto type = node_type_from(type_name);
    if (!type.has_value()) {
        errors.push_back("node \"" + node.id + "\": unknown type \"" + type_name + "\"");
        return node;
    }
    node.type = *type;
    if (node.type == ntp::NodeType::kNativeStage) {
        errors.push_back("node \"" + node.id +
                         "\": native_stage is reserved for a future C-ABI stage and is not "
                         "loadable in NTP v1");
    }
    const std::string scope = j.value("scope", is_instrument ? "voice" : "shared");
    node.scope = scope == "voice" ? ntp::NodeScope::kVoice : ntp::NodeScope::kShared;
    if (!is_instrument) {
        node.scope = ntp::NodeScope::kShared; // FX graphs are all-shared
    }

    node.gain = j.value("gain", node.gain);
    node.delay_time = j.value("delayTime", node.delay_time);
    node.max_delay = j.value("maxDelay", node.max_delay);
    node.filter_type = j.value("filterType", node.filter_type);
    node.frequency = j.value("frequency", node.frequency);
    node.q = j.value("q", j.value("Q", node.q));
    node.threshold = j.value("threshold", node.threshold);
    node.ratio = j.value("ratio", node.ratio);
    node.attack = j.value("attack", node.attack);
    node.release = j.value("release", node.release);
    node.knee = j.value("knee", node.knee);
    node.pan = j.value("pan", node.pan);
    node.shaper_curve = j.value("curve", node.shaper_curve);
    node.drive = j.value("drive", node.drive);
    node.osc_type = j.value("oscType", node.osc_type);
    node.noise_type = j.value("noiseType", node.noise_type);
    node.constant_value = j.value("value", node.constant_value);
    node.lfo_shape = j.value("lfoShape", node.lfo_shape);
    node.lfo_rate = j.value("lfoRate", node.lfo_rate);
    node.lfo_depth = j.value("lfoDepth", node.lfo_depth);
    node.sample_file = j.value("sampleFile", node.sample_file);
    node.playback_mode = j.value("playbackMode", node.playback_mode);
    node.grain_envelope = j.value("grainEnvelope", node.grain_envelope);
    node.table_file = j.value("tableFile", node.table_file);
    node.frame_count = j.value("frameCount", node.frame_count);
    node.interpolation_linear = j.value("interpolation", "linear") != std::string("none");
    node.impulse = j.value("impulse", node.impulse);
    node.normalize = j.value("normalize", node.normalize);
    node.polyphony = j.value("polyphony", node.polyphony);
    node.voice_stealing = j.value("voiceStealing", node.voice_stealing);

    for (const json& stage : j.value("envStages", json::array())) {
        ntp::EnvelopeStage s;
        s.target = stage.value("target", 0.0);
        s.time = stage.value("time", 0.0);
        s.exponential = stage.value("curve", "linear") == std::string("exponential");
        node.env_stages.push_back(s);
    }
    if (node.type == ntp::NodeType::kEnvelope && node.env_stages.empty()) {
        errors.push_back("node \"" + node.id + "\": envelope needs envStages");
    }

    for (const json& zj : j.value("zones", json::array())) {
        ntp::SampleZone zone;
        zone.file = zj.value("file", "");
        zone.root_key = zj.value("rootKey", zone.root_key);
        if (zj.contains("keyRange")) {
            zone.key_lo = zj["keyRange"].value("lo", 0);
            zone.key_hi = zj["keyRange"].value("hi", 127);
        }
        if (zj.contains("velocityRange")) {
            zone.vel_lo = zj["velocityRange"].value("lo", 1);
            zone.vel_hi = zj["velocityRange"].value("hi", 127);
        }
        if (zj.contains("loop")) {
            zone.loop = loop_mode_from(zj["loop"]);
        }
        zone.loop_start = zj.value("loopStart", 0.0);
        zone.loop_end = zj.value("loopEnd", 0.0);
        zone.start_offset = zj.value("startOffset", 0.0);
        zone.duration = zj.value("duration", 0.0);
        zone.pitch_tracking = zj.value("pitchTracking", true);
        zone.round_robin_group = zj.value("roundRobinGroup", "");
        zone.choke_group = zj.value("choke", "");
        zone.release_trigger = zj.value("trigger", "attack") == std::string("release");
        zone.user_assignable = zj.value("userAssignable", false);
        zone.slot_id = zj.value("slotId", "");
        zone.slot_label = zj.value("slotLabel", "");
        zone.fallback_file = zj.value("fallbackFile", "");
        zone.max_duration_sec = zj.value("maxDurationSec", 0.0);
        if (zone.user_assignable) {
            // Strict slot contract: a stable id (POVR keys on it) and
            // an authored default (a fresh install must never be
            // silent). The web only "strongly recommended" the
            // fallback; native refuses (fix-don't-retain).
            if (zone.slot_id.empty()) {
                errors.push_back("node \"" + node.id + "\": user-assignable zone needs a slotId");
            }
            if (zone.fallback_file.empty()) {
                errors.push_back("node \"" + node.id + "\": user-assignable zone \"" +
                                 zone.slot_id + "\" needs a fallbackFile");
            }
        } else if (zone.file.empty()) {
            errors.push_back("node \"" + node.id + "\": sampler zone without a file");
        }
        node.zones.push_back(std::move(zone));
    }

    if (node.type == ntp::NodeType::kSampler && j.contains("sliceMap")) {
        node.slice_map = parse_slice_map(j["sliceMap"], node.id, errors);
    }
    if (node.type == ntp::NodeType::kSampler && node.zones.empty() && !node.slice_map.has_value()) {
        errors.push_back("node \"" + node.id + "\": sampler needs at least one zone or a sliceMap");
    }
    if (node.type == ntp::NodeType::kGranular && node.sample_file.empty()) {
        errors.push_back("node \"" + node.id + "\": granular needs sampleFile");
    }
    if (node.type == ntp::NodeType::kWavetable && node.table_file.empty()) {
        errors.push_back("node \"" + node.id + "\": wavetable needs tableFile");
    }
    if (node.type == ntp::NodeType::kConvolver && node.impulse.empty()) {
        errors.push_back("node \"" + node.id + "\": convolver needs impulse");
    }
    return node;
}

// Control trees are recursive by design (bounded to 4 levels above).
// NOLINTNEXTLINE(misc-no-recursion)
ntp::UiControl parse_control(const json& j, const ntp::Manifest& manifest,
                             std::vector<std::string>& errors, ntp::UiDef& ui, int depth) {
    ntp::UiControl control;
    const std::string type_name = j.value("type", "");
    if (type_name == "webview" || type_name == "waveform_view") {
        // No webview in NTP; waveform_view was webview-era chrome. The
        // control is skipped and the caller falls back appropriately.
        ui.had_webview = true;
        control.type = ntp::ControlType::kLabel;
        control.label = "";
        return control;
    }
    const auto type = control_type_from(type_name);
    if (!type.has_value()) {
        errors.push_back("ui control with unknown type \"" + type_name + "\"");
        return control;
    }
    control.type = *type;
    control.parameter = j.value("parameter", "");
    control.parameter_x = j.value("parameterX", "");
    control.parameter_y = j.value("parameterY", "");
    control.label = j.value("label", "");
    control.node = j.value("analyserNode", j.value("node", ""));
    control.asset = j.value("asset", "");
    control.row_layout = j.value("style", "row") != std::string("column");
    control.width = j.value("width", 0.0F);
    control.height = j.value("height", 0.0F);
    for (const json& option : j.value("options", json::array())) {
        if (option.is_string()) {
            control.options.push_back(option.get<std::string>());
        }
    }

    auto param_exists = [&manifest](const std::string& key) {
        return std::any_of(manifest.params.begin(), manifest.params.end(),
                           [&key](const ntp::ParamDef& p) { return p.key == key; });
    };
    for (const std::string& key : {control.parameter, control.parameter_x, control.parameter_y}) {
        if (!key.empty() && !param_exists(key)) {
            errors.push_back("ui control references unknown parameter \"" + key + "\"");
        }
    }
    if (control.type == ntp::ControlType::kSelect && control.options.empty()) {
        errors.emplace_back("select control needs options");
    }

    if (control.type == ntp::ControlType::kGroup) {
        if (depth >= 4) {
            errors.emplace_back("ui groups nest deeper than 4 levels");
        } else {
            for (const json& child : j.value("children", json::array())) {
                control.children.push_back(parse_control(child, manifest, errors, ui, depth + 1));
            }
        }
    }
    return control;
}

} // namespace

bool parse_ntp_manifest(const std::string& json_text, ntp::Manifest& manifest,
                        std::vector<std::string>& errors) {
    const json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        errors.emplace_back("plugin.json is not valid JSON");
        return false;
    }

    if (!root.contains("ntp") || !root["ntp"].is_number_integer() ||
        root["ntp"].get<int>() != ntp::kSchemaVersion) {
        errors.emplace_back("missing or unsupported \"ntp\" schema version (this host reads 1); "
                            "web-format manifests must go through ntp-convert");
    }
    manifest.ntp = ntp::kSchemaVersion;
    manifest.id = root.value("id", "");
    manifest.name = root.value("name", "");
    manifest.version = root.value("version", "1.0.0");
    manifest.author = root.value("author", "");
    manifest.description = root.value("description", "");
    if (manifest.id.empty()) {
        errors.emplace_back("missing plugin id");
    }
    if (manifest.name.empty()) {
        errors.emplace_back("missing plugin name");
    }
    const std::string type = root.value("type", "");
    if (type == "instrument") {
        manifest.type = ntp::PluginType::kInstrument;
    } else if (type == "fx") {
        manifest.type = ntp::PluginType::kFx;
    } else {
        errors.push_back(R"(type must be "instrument" or "fx" (got ")" + type + "\")");
    }
    const bool is_instrument = manifest.type == ntp::PluginType::kInstrument;

    for (const std::string& requirement : root.value("requires", std::vector<std::string>{})) {
        // "userSamples" (user-assignable slots + POVR persistence) is
        // the one v1 capability; anything else is from a newer host or
        // the unconverted web format. Strict refusal.
        if (requirement != "userSamples") {
            errors.push_back("unknown capability requirement \"" + requirement + "\"");
        }
        manifest.required_caps.push_back(requirement);
    }

    std::set<std::string> param_keys;
    for (const json& pj : root.value("params", json::array())) {
        ntp::ParamDef param;
        param.key = pj.value("key", "");
        param.label = pj.value("label", param.key);
        param.min = pj.value("min", 0.0);
        param.max = pj.value("max", 1.0);
        param.def = pj.value("default", param.min);
        param.step = pj.value("step", 0.0);
        param.unit = pj.value("unit", "");
        param.display_decimals = pj.value("displayDecimals", 2);
        param.group = pj.value("group", "");
        param.curve = curve_from(pj.value("curve", "linear"));
        param.midi_learnable = pj.value("midiLearnable", true);
        if (param.key.empty()) {
            errors.emplace_back("parameter without a key");
        } else if (!param_keys.insert(param.key).second) {
            errors.push_back("duplicate parameter key \"" + param.key + "\"");
        }
        if (param.min >= param.max) {
            errors.push_back("parameter \"" + param.key + "\": min must be < max");
        }
        if (param.def < param.min || param.def > param.max) {
            errors.push_back("parameter \"" + param.key + "\": default outside range");
        }
        manifest.params.push_back(std::move(param));
    }

    manifest.voices = std::clamp(root.value("voices", 8), 1, 64);
    manifest.voice_stealing = root.value("voiceStealing", "oldest");
    manifest.release_tail = root.value("releaseTail", 8.0);

    // ── Graph ────────────────────────────────────────────────────────
    const json graph = root.value("graph", json::object());
    std::set<std::string> node_ids;
    for (const json& nj : graph.value("nodes", json::array())) {
        ntp::DspNode node = parse_node(nj, errors, is_instrument);
        if (!node.id.empty() && !node_ids.insert(node.id).second) {
            errors.push_back("duplicate node id \"" + node.id + "\"");
        }
        manifest.graph.nodes.push_back(std::move(node));
    }

    // Plugin-wide slot checks: ids are the POVR persistence keys, so
    // they must be unique across every sampler node, and slot-sourced
    // slice maps must reference a slot that exists.
    const bool has_user_samples_cap =
        std::find(manifest.required_caps.begin(), manifest.required_caps.end(), "userSamples") !=
        manifest.required_caps.end();
    std::set<std::string> slot_ids;
    for (const ntp::DspNode& node : manifest.graph.nodes) {
        for (const ntp::SampleZone& zone : node.zones) {
            if (!zone.user_assignable || zone.slot_id.empty()) {
                continue;
            }
            if (!slot_ids.insert(zone.slot_id).second) {
                errors.push_back("node \"" + node.id + "\": duplicate slot id \"" + zone.slot_id +
                                 "\"");
            }
            if (!has_user_samples_cap) {
                errors.push_back("node \"" + node.id + "\": user-assignable zone \"" +
                                 zone.slot_id + R"(" needs "userSamples" in requires[])");
            }
        }
    }
    for (const ntp::DspNode& node : manifest.graph.nodes) {
        if (!node.slice_map.has_value() || !node.slice_map->source.starts_with("slotId:")) {
            continue;
        }
        const std::string slot = node.slice_map->source.substr(7);
        if (!slot_ids.contains(slot)) {
            errors.push_back("node \"" + node.id +
                             "\": sliceMap source references unknown slot \"" + slot + "\"");
        }
    }
    auto endpoint_ok = [&](const std::string& name, bool as_source) {
        if (node_ids.contains(name)) {
            return true;
        }
        if (is_instrument) {
            return as_source ? name == "voiceIn" : name == "voiceOut" || name == "output";
        }
        return as_source ? name == "input" : name == "output";
    };
    for (const json& cj : graph.value("connections", json::array())) {
        ntp::Connection connection;
        connection.from = cj.value("from", "");
        connection.to = cj.value("to", "");
        connection.to_param = cj.value("toParam", "");
        if (!endpoint_ok(connection.from, true)) {
            errors.push_back("connection from unknown endpoint \"" + connection.from + "\"");
        }
        if (!connection.to_param.empty()) {
            if (!node_ids.contains(connection.to)) {
                errors.push_back("param connection to unknown node \"" + connection.to + "\"");
            }
        } else if (!endpoint_ok(connection.to, false)) {
            errors.push_back("connection to unknown endpoint \"" + connection.to + "\"");
        }
        manifest.graph.connections.push_back(std::move(connection));
    }
    for (const json& rj : graph.value("modRoutes", json::array())) {
        ntp::ModRoute route;
        route.source = rj.value("source", "");
        const bool source_ok =
            node_ids.contains(route.source) || route.source == "velocity" ||
            route.source == "note" || route.source == "gate" || route.source == "pitch" ||
            (route.source.starts_with("param:") && param_keys.contains(route.source.substr(6)));
        if (!source_ok) {
            errors.push_back("mod route from unknown source \"" + route.source + "\"");
        }
        for (const json& tj : rj.value("targets", json::array())) {
            ntp::ModTarget target;
            target.target = tj.value("target", "");
            target.depth = tj.value("depth", 1.0);
            target.offset = tj.value("offset", 0.0);
            target.scale = tj.value("scale", 1.0);
            target.slew = tj.value("slew", 0.0);
            const auto transform = transform_from(tj.value("transform", "none"));
            if (!transform.has_value()) {
                errors.push_back("mod target \"" + target.target + "\": unknown transform");
            } else {
                target.transform = *transform;
            }
            target.curve = curve_from(tj.value("curve", "linear"));
            const auto dot = target.target.find('.');
            if (dot == std::string::npos || !node_ids.contains(target.target.substr(0, dot))) {
                errors.push_back("mod route targets unknown node in \"" + target.target + "\"");
            }
            route.targets.push_back(std::move(target));
        }
        if (route.targets.empty()) {
            errors.push_back("mod route from \"" + route.source + "\" has no targets");
        }
        manifest.graph.mod_routes.push_back(std::move(route));
    }

    // ── Ports ────────────────────────────────────────────────────────
    const json ports = root.value("ports", json::object());
    auto parse_ports = [&errors](const json& list, std::vector<ntp::PortDef>& out,
                                 const char* side) {
        std::set<std::string> ids;
        for (const json& pj : list) {
            ntp::PortDef port;
            port.id = pj.value("id", "");
            port.label = pj.value("label", port.id);
            const auto kind = port_kind_from(pj.value("kind", "audio"));
            if (!kind.has_value()) {
                errors.push_back(std::string("port \"") + port.id + "\" on " + side +
                                 ": unknown kind");
            } else {
                port.kind = *kind;
            }
            if (port.id.empty()) {
                errors.push_back(std::string("port without id on ") + side);
            } else if (!ids.insert(port.id).second) {
                errors.push_back("duplicate port id \"" + port.id + "\" on " + side);
            }
            out.push_back(std::move(port));
        }
    };
    parse_ports(ports.value("inputs", json::array()), manifest.inputs, "inputs");
    parse_ports(ports.value("outputs", json::array()), manifest.outputs, "outputs");
    // Every plugin gets a main audio out if it declared none — a
    // plugin with no output cannot exist in a patchbay.
    if (manifest.outputs.empty()) {
        manifest.outputs.push_back({.id = "main", .label = "OUT", .kind = ntp::PortKind::kAudio});
    }

    // ── UI ───────────────────────────────────────────────────────────
    for (const json& cj : root.value("ui", json::object()).value("controls", json::array())) {
        ntp::UiControl control = parse_control(cj, manifest, errors, manifest.ui, 0);
        if (control.type != ntp::ControlType::kLabel || !control.label.empty()) {
            manifest.ui.controls.push_back(std::move(control));
        }
    }

    // ── Presets ──────────────────────────────────────────────────────
    for (const json& pj : root.value("presets", json::array())) {
        ntp::Preset preset;
        preset.name = pj.value("name", "");
        if (preset.name.empty()) {
            errors.emplace_back("preset without a name");
        }
        // Bind before .items(): the sub-expression temporary would not
        // outlive the loop (C++20; P2718 fixes this only in C++23).
        const json values = pj.value("values", json::object());
        for (const auto& [key, value] : values.items()) {
            if (!param_keys.contains(key)) {
                errors.push_back("preset \"" + preset.name + "\" sets unknown parameter \"" + key +
                                 "\"");
                continue;
            }
            preset.values.emplace_back(key, value.is_number() ? value.get<double>() : 0.0);
        }
        manifest.presets.push_back(std::move(preset));
    }

    return errors.empty();
}

namespace {

// Case-normalised archive lookup: authors zip on varied platforms and
// path case is the classic portability trap.
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct ArchiveFiles {
    std::map<std::string, std::vector<std::uint8_t>> by_path; // lowercased paths
};

bool read_zip(const std::uint8_t* data, std::size_t size, ArchiveFiles& out,
              std::vector<std::string>& errors) {
    mz_zip_archive zip{};
    if (mz_zip_reader_init_mem(&zip, data, size, 0) == MZ_FALSE) {
        errors.emplace_back("archive is not a readable ZIP file");
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (mz_zip_reader_file_stat(&zip, i, &stat) == MZ_FALSE ||
            mz_zip_reader_is_file_a_directory(&zip, i) == MZ_TRUE) {
            continue;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
        if (mz_zip_reader_extract_to_mem(&zip, i, bytes.data(), bytes.size(), 0) == MZ_FALSE) {
            errors.push_back(std::string("archive entry unreadable: ") +
                             static_cast<const char*>(stat.m_filename));
            continue;
        }
        out.by_path[lower(static_cast<const char*>(stat.m_filename))] = std::move(bytes);
    }
    mz_zip_reader_end(&zip);
    return true;
}

const std::vector<std::uint8_t>* find_asset(const ArchiveFiles& files, const std::string& path) {
    const auto it = files.by_path.find(lower(path));
    return it != files.by_path.end() ? &it->second : nullptr;
}

} // namespace

std::unique_ptr<LoadedNtpPlugin> load_ntp_archive(const std::uint8_t* data, std::size_t size,
                                                  std::uint32_t device_rate,
                                                  std::vector<std::string>& errors) {
    ArchiveFiles files;
    if (!read_zip(data, size, files, errors)) {
        return nullptr;
    }
    const std::vector<std::uint8_t>* manifest_bytes = find_asset(files, "plugin.json");
    if (manifest_bytes == nullptr) {
        errors.emplace_back("archive has no plugin.json");
        return nullptr;
    }

    auto plugin = std::make_unique<LoadedNtpPlugin>();
    plugin->archive_bytes.assign(data, data + size);
    const std::string manifest_text(manifest_bytes->begin(), manifest_bytes->end());
    if (!parse_ntp_manifest(manifest_text, plugin->manifest, errors)) {
        return nullptr;
    }
    if (plugin->manifest.ui.had_webview && plugin->manifest.ui.controls.empty()) {
        plugin->warnings.emplace_back(
            "plugin declares only a webview UI — showing the auto-generated parameter panel");
    }

    // ── Assets ───────────────────────────────────────────────────────
    auto decode_sample = [&](const std::string& path, const char* what,
                             const std::string& node_id) {
        if (plugin->samples.contains(path)) {
            return;
        }
        const std::vector<std::uint8_t>* bytes = find_asset(files, path);
        if (bytes == nullptr) {
            errors.push_back("node \"" + node_id + "\": " + what + " \"" + path +
                             "\" not in archive");
            return;
        }
        std::string format;
        std::string decode_error;
        auto buffer = audio::load_sample_memory(bytes->data(), bytes->size(), device_rate, format,
                                                decode_error);
        if (buffer == nullptr) {
            errors.push_back("node \"" + node_id + "\": " + what + " \"" + path +
                             "\": " + decode_error);
            return;
        }
        plugin->samples[path] = std::move(buffer);
    };

    auto decode_table = [&](const std::string& path, const std::string& node_id) {
        if (plugin->tables.contains(path)) {
            return;
        }
        const std::vector<std::uint8_t>* bytes = find_asset(files, path);
        if (bytes == nullptr) {
            errors.push_back("node \"" + node_id + "\": tableFile \"" + path + "\" not in archive");
            return;
        }
        // Wavetables stay at source rate and mono: the frames are what
        // matter, and resampling would smear the frame boundaries.
        audio::codec::Decoded decoded;
        std::string decode_error;
        const bool ok =
            audio::codec::decode_wav(bytes->data(), bytes->size(), decoded, decode_error) ||
            audio::codec::decode_ogg(bytes->data(), bytes->size(), decoded, decode_error) ||
            audio::codec::decode_mp3(bytes->data(), bytes->size(), decoded, decode_error);
        if (!ok) {
            errors.push_back("node \"" + node_id + "\": tableFile \"" + path + "\" undecodable");
            return;
        }
        LoadedNtpPlugin::RawTable table;
        table.rate = decoded.rate;
        table.mono.resize(decoded.frames);
        for (std::uint32_t f = 0; f < decoded.frames; ++f) {
            float sum = 0.0F;
            for (std::uint32_t c = 0; c < decoded.channels; ++c) {
                sum += decoded.interleaved[(static_cast<std::size_t>(f) * decoded.channels) + c];
            }
            table.mono[f] = sum / static_cast<float>(std::max(1U, decoded.channels));
        }
        plugin->tables[path] = std::move(table);
    };

    for (const ntp::DspNode& node : plugin->manifest.graph.nodes) {
        switch (node.type) {
        case ntp::NodeType::kGranular:
            decode_sample(node.sample_file, "sampleFile", node.id);
            break;
        case ntp::NodeType::kWavetable:
            decode_table(node.table_file, node.id);
            break;
        case ntp::NodeType::kConvolver:
            decode_sample(node.impulse, "impulse", node.id);
            break;
        case ntp::NodeType::kSampler:
            for (const ntp::SampleZone& zone : node.zones) {
                // User-assignable zones may author only a fallback;
                // both decode when both exist (fallback is what plays
                // until the user assigns).
                if (!zone.file.empty()) {
                    decode_sample(zone.file, "zone file", node.id);
                }
                if (!zone.fallback_file.empty()) {
                    decode_sample(zone.fallback_file, "fallbackFile", node.id);
                }
            }
            if (node.slice_map.has_value() && !node.slice_map->source.empty() &&
                !node.slice_map->source.starts_with("slotId:")) {
                decode_sample(node.slice_map->source, "sliceMap source", node.id);
            }
            break;
        default:
            break;
        }
    }

    // ── Slice-map grid expansion ─────────────────────────────────────
    // "grid:N" needs the source duration, so it resolves here — after
    // asset decode — into N equal author-shaped slices. The runtime
    // then never distinguishes derived from authored slices.
    for (ntp::DspNode& node : plugin->manifest.graph.nodes) {
        if (node.type != ntp::NodeType::kSampler || !node.slice_map.has_value()) {
            continue;
        }
        ntp::SliceMap& map = *node.slice_map;
        const std::optional<int> grid = grid_slice_count(map.auto_detect);
        if (!grid.has_value()) {
            continue; // author-supplied slices (validation caught the rest)
        }
        std::string source_path = map.source;
        if (source_path.starts_with("slotId:")) {
            const std::string slot = source_path.substr(7);
            source_path.clear();
            // Slot ids are plugin-wide (validated above), so the owning
            // zone may live on another sampler node.
            for (const ntp::DspNode& other : plugin->manifest.graph.nodes) {
                for (const ntp::SampleZone& zone : other.zones) {
                    if (zone.user_assignable && zone.slot_id == slot) {
                        source_path = zone.fallback_file.empty() ? zone.file : zone.fallback_file;
                        break;
                    }
                }
            }
        }
        const auto it = plugin->samples.find(source_path);
        if (it == plugin->samples.end() || it->second->rate == 0) {
            errors.push_back("node \"" + node.id + "\": sliceMap grid source \"" + map.source +
                             "\" has no decodable audio");
            continue;
        }
        const double duration =
            static_cast<double>(it->second->frames) / static_cast<double>(it->second->rate);
        map.slices.reserve(static_cast<std::size_t>(*grid));
        for (int i = 0; i < *grid; ++i) {
            ntp::SliceEntry slice;
            slice.start = duration * i / *grid;
            slice.end = duration * (i + 1) / *grid;
            map.slices.push_back(slice);
        }
    }

    // UI images (image/sprite controls and sprite knobs).
    // NOLINTNEXTLINE(misc-no-recursion) — bounded control-tree walk
    auto collect_images = [&](const ntp::UiControl& control, auto&& recurse) -> void {
        if (!control.asset.empty() && !plugin->images.contains(control.asset)) {
            const std::vector<std::uint8_t>* bytes = find_asset(files, control.asset);
            if (bytes == nullptr) {
                errors.push_back("ui asset \"" + control.asset + "\" not in archive");
            } else {
                LoadedNtpPlugin::Image image;
                std::string image_error;
                if (!decode_image_rgba(bytes->data(), bytes->size(), image.width, image.height,
                                       image.rgba, image_error)) {
                    errors.push_back("ui asset \"" + control.asset + "\": " + image_error);
                } else {
                    plugin->images[control.asset] = std::move(image);
                }
            }
        }
        for (const ntp::UiControl& child : control.children) {
            recurse(child, recurse);
        }
    };
    for (const ntp::UiControl& control : plugin->manifest.ui.controls) {
        collect_images(control, collect_images);
    }

    if (!errors.empty()) {
        return nullptr;
    }
    return plugin;
}

std::unique_ptr<LoadedNtpPlugin> load_ntp_file(const std::string& path, std::uint32_t device_rate,
                                               std::vector<std::string>& errors) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        errors.push_back("cannot open " + path);
        return nullptr;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    return load_ntp_archive(bytes.data(), bytes.size(), device_rate, errors);
}

} // namespace nt::plugins
