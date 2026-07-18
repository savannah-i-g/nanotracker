// api/local_api — protocol triage (server threads) and command
// dispatch (UI thread). See local_api.h for the wire protocol and the
// threading contract. Dispatch is two-phase like the web executor
// (trackerLocalApi.ts): the whole batch validates against pre-batch
// state first, then applies; native I/O commands (project load, sample
// upload, export) can still fail at apply time, in which case the
// reply reports the failing index and how many commands landed before
// it — a case the web surface never had.
#include "api/local_api.h"

#include "app/undo.h"
#include "app/version.h"
#include "engine/tracker_types.h"
#include "graph/graph_model.h"
#include "io/export_render.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <random>
#include <utility>

namespace nt::api {

namespace {

using nlohmann::json;

// Error codes: the web's LocalApiErrorCode strings, plus native
// additions "unauthorized" (auth handshake) and "unsupported" (web
// ops whose session surface does not exist in the native port yet —
// a typed refusal, never a silent accept).
constexpr const char* kErrInvalidOp = "invalidOp";
constexpr const char* kErrInvalidField = "invalidField";
constexpr const char* kErrOutOfBounds = "outOfBounds";
constexpr const char* kErrNotFound = "notFound";
constexpr const char* kErrLimitExceeded = "limitExceeded";
constexpr const char* kErrPayloadTooLarge = "payloadTooLarge";
constexpr const char* kErrMissingUndoDescription = "missingUndoDescription";
constexpr const char* kErrRateLimited = "rateLimited";
constexpr const char* kErrIoError = "ioError";
constexpr const char* kErrUnauthorized = "unauthorized";
constexpr const char* kErrUnsupported = "unsupported";

json make_error(int index, const char* code, const std::string& message) {
    return json{{"index", index}, {"code", code}, {"message", message}};
}

json failure(json error) {
    return json{{"ok", false}, {"errors", json::array({std::move(error)})}};
}

// ── Checked JSON field readers ───────────────────────────────────────
// Untrusted input: every access type-checks. nlohmann's value() throws
// on present-but-mistyped keys, so it is never used on request bodies.

bool get_int(const json& j, const char* key, int& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) {
        return false;
    }
    out = it->get<int>();
    return true;
}

bool get_double(const json& j, const char* key, double& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) {
        return false;
    }
    out = it->get<double>();
    return true;
}

bool get_string(const json& j, const char* key, std::string& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool get_bool(const json& j, const char* key, bool& out) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) {
        return false;
    }
    out = it->get<bool>();
    return true;
}

// ── Model lookups ────────────────────────────────────────────────────

int pattern_index_by_id(const engine::TrackerProject& project, int id) {
    for (std::size_t i = 0; i < project.patterns.size(); ++i) {
        if (project.patterns[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const engine::TrackerSample* sample_by_id(const engine::TrackerProject& project, int id) {
    for (const engine::TrackerSample& sample : project.samples) {
        if (sample.id == id) {
            return &sample;
        }
    }
    return nullptr;
}

// Existing-only view of a sequence layer (never grows containers —
// validation must not mutate).
const engine::SequenceLayer* peek_seq_layer(const engine::TrackerProject& project,
                                            int pattern_index, int channel, int layer) {
    const auto& seq = project.sequence_mixer.seq_patterns;
    if (pattern_index < 0 || pattern_index >= static_cast<int>(seq.size())) {
        return nullptr;
    }
    const auto& layers = seq[static_cast<std::size_t>(pattern_index)].layers;
    if (channel < 0 || channel >= static_cast<int>(layers.size())) {
        return nullptr;
    }
    const auto& channel_layers = layers[static_cast<std::size_t>(channel)];
    if (layer < 0 || layer >= static_cast<int>(channel_layers.size())) {
        return nullptr;
    }
    return &channel_layers[static_cast<std::size_t>(layer)];
}

// ── JSON ⇄ model conversions (web field names kept verbatim) ─────────

std::uint8_t clamp_byte(double v) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v), 0, 255));
}

// Merges present cell fields onto `cell` (web setCell partial-merge
// semantics). Returns false with the offending key in `bad_field`.
bool merge_cell_fields(const json& src, engine::TrackerCell& cell, std::string& bad_field) {
    struct Field {
        const char* key;
        std::uint8_t* dest;
    };

    const std::array<Field, 6> fields = {{
        {"note", &cell.note},
        {"instrument", &cell.instrument},
        {"volume", &cell.volume},
        {"effect", &cell.effect},
        {"effectParam", &cell.effect_param},
        {"boundIndex", &cell.bound_index},
    }};
    for (const Field& field : fields) {
        const auto it = src.find(field.key);
        if (it == src.end()) {
            continue;
        }
        if (!it->is_number()) {
            bad_field = field.key;
            return false;
        }
        *field.dest = clamp_byte(it->get<double>());
    }
    return true;
}

json cell_to_json(const engine::TrackerCell& cell) {
    return json{{"note", cell.note},
                {"instrument", cell.instrument},
                {"volume", cell.volume},
                {"effect", cell.effect},
                {"effectParam", cell.effect_param},
                {"boundIndex", cell.bound_index}};
}

json note_to_json(const engine::SequenceNote& note) {
    return json{{"pitch", note.pitch},
                {"startTick", note.start_tick},
                {"durationTicks", note.duration_ticks},
                {"velocity", note.velocity}};
}

bool note_from_json(const json& src, engine::SequenceNote& note) {
    return get_int(src, "pitch", note.pitch) && get_int(src, "startTick", note.start_tick) &&
           get_int(src, "durationTicks", note.duration_ticks) &&
           get_int(src, "velocity", note.velocity);
}

const char* instrument_type_name(engine::InstrumentSourceType type) {
    switch (type) {
    case engine::InstrumentSourceType::kSample:
        return "sample";
    case engine::InstrumentSourceType::kPlugin:
        return "plugin";
    case engine::InstrumentSourceType::kWorkspace:
        return "workspace";
    }
    return "sample";
}

const char* node_kind_name(graph::NodeKind kind) {
    switch (kind) {
    case graph::NodeKind::kTrackerBus:
        return "trackerBus";
    case graph::NodeKind::kMasterIn:
        return "masterIn";
    case graph::NodeKind::kModulePlayer:
        return "modulePlayer";
    case graph::NodeKind::kUtilitySum:
        return "sum";
    case graph::NodeKind::kPlugin:
        return "plugin";
    case graph::NodeKind::kExtMidiIn:
        return "extMidiIn";
    case graph::NodeKind::kExtMidiOut:
        return "extMidiOut";
    }
    return "sum";
}

json ports_to_json(const std::vector<graph::Port>& ports) {
    json out = json::array();
    for (const graph::Port& port : ports) {
        out.push_back(json{
            {"id", port.id}, {"label", port.label}, {"kind", graph::port_kind_name(port.kind)}});
    }
    return out;
}

// ── Base64 (sample upload payloads) ──────────────────────────────────
// Strict standard-alphabet decoder: whitespace is skipped, '=' only as
// terminal padding, anything else rejects — a corrupt upload must fail
// loudly, never decode to garbage audio.

bool base64_decode(const std::string& text, std::vector<std::uint8_t>& out, std::string& error) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };

    out.clear();
    out.reserve((text.size() / 4) * 3);
    std::uint32_t accum = 0;
    int bits = 0;
    bool padded = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c == '=') {
            padded = true;
            continue;
        }
        if (padded) {
            error = "base64: data after padding";
            return false;
        }
        const int value = value_of(c);
        if (value < 0) {
            error = std::string("base64: invalid character '") + c + "'";
            return false;
        }
        accum = (accum << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(
                static_cast<std::uint8_t>((accum >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    if (out.empty()) {
        error = "base64: empty payload";
        return false;
    }
    return true;
}

// Keeps only filename-safe characters of a client-supplied sample name
// (the byte payload lands in a temp file that flows through the
// standard slot-load path, which derives the sample name from the
// file stem).
std::string sanitize_upload_name(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '.' || c == '-' ||
            c == '_') {
            out.push_back(c);
        }
    }
    if (out.empty() || out.front() == '.') {
        out = "upload" + out;
    }
    return out;
}

// ── Option-string parsers (export) ───────────────────────────────────

bool parse_export_format(const std::string& text, io::ExportFormat& out) {
    if (text == "wav") {
        out = io::ExportFormat::kWav;
    } else if (text == "ogg") {
        out = io::ExportFormat::kOgg;
    } else if (text == "mp3") {
        out = io::ExportFormat::kMp3;
    } else {
        return false;
    }
    return true;
}

bool parse_wav_depth(const std::string& text, io::WavDepth& out) {
    if (text == "pcm16") {
        out = io::WavDepth::kPcm16;
    } else if (text == "pcm24") {
        out = io::WavDepth::kPcm24;
    } else if (text == "float32") {
        out = io::WavDepth::kFloat32;
    } else {
        return false;
    }
    return true;
}

bool parse_fade_shape(const std::string& text, io::FadeShape& out) {
    if (text == "linear") {
        out = io::FadeShape::kLinear;
    } else if (text == "equalPower") {
        out = io::FadeShape::kEqualPower;
    } else {
        return false;
    }
    return true;
}

bool parse_normalize_mode(const std::string& text, io::NormalizeMode& out) {
    if (text == "none") {
        out = io::NormalizeMode::kNone;
    } else if (text == "peak") {
        out = io::NormalizeMode::kPeak;
    } else if (text == "truePeak") {
        out = io::NormalizeMode::kTruePeak;
    } else if (text == "lufs") {
        out = io::NormalizeMode::kLufs;
    } else {
        return false;
    }
    return true;
}

// Builds io::ExportOptions from the exportProject argument object.
// Absent fields keep defaults; present-but-invalid fields error.
bool parse_export_options(const json& cmd, io::ExportOptions& options, std::string& message) {
    std::string text;
    if (get_string(cmd, "format", text) && !parse_export_format(text, options.format)) {
        message = "format must be wav|ogg|mp3";
        return false;
    }
    if (get_string(cmd, "wavDepth", text) && !parse_wav_depth(text, options.wav_depth)) {
        message = "wavDepth must be pcm16|pcm24|float32";
        return false;
    }
    double number = 0.0;
    if (get_double(cmd, "oggQuality", number)) {
        if (number < -0.1 || number > 1.0) {
            message = "oggQuality must be -0.1..1.0";
            return false;
        }
        options.ogg_quality = static_cast<float>(number);
    }
    int integer = 0;
    if (get_int(cmd, "mp3BitrateKbps", integer)) {
        options.mp3_bitrate_kbps = integer;
    }
    if (get_int(cmd, "mp3Quality", integer)) {
        if (integer < 0 || integer > 9) {
            message = "mp3Quality must be 0..9";
            return false;
        }
        options.mp3_quality = integer;
    }
    if (get_int(cmd, "sampleRate", integer)) {
        if (integer < 8000 || integer > 192000) {
            message = "sampleRate must be 8000..192000";
            return false;
        }
        options.sample_rate = static_cast<std::uint32_t>(integer);
    }
    if (get_double(cmd, "tailSeconds", number)) {
        if (number < 0.0 || number > 60.0) {
            message = "tailSeconds must be 0..60";
            return false;
        }
        options.tail_seconds = number;
    }
    if (get_int(cmd, "startOrder", integer)) {
        options.start_order = integer;
    }
    if (get_int(cmd, "endOrder", integer)) {
        options.end_order = integer;
    }
    if (get_int(cmd, "stemMask", integer)) {
        options.stem_mask = static_cast<std::uint32_t>(integer);
    }
    bool flag = false;
    if (get_bool(cmd, "stemZip", flag)) {
        options.stem_zip = flag;
    }
    if (const auto it = cmd.find("metadata"); it != cmd.end()) {
        if (!it->is_object()) {
            message = "metadata must be an object";
            return false;
        }
        get_string(*it, "title", options.metadata.title);
        get_string(*it, "artist", options.metadata.artist);
        get_string(*it, "album", options.metadata.album);
        get_string(*it, "date", options.metadata.date);
        get_string(*it, "comment", options.metadata.comment);
    }
    if (const auto it = cmd.find("post"); it != cmd.end()) {
        if (!it->is_object()) {
            message = "post must be an object";
            return false;
        }
        get_double(*it, "fadeInSeconds", options.post.fade_in_seconds);
        get_double(*it, "fadeOutSeconds", options.post.fade_out_seconds);
        if (get_string(*it, "fadeInShape", text) &&
            !parse_fade_shape(text, options.post.fade_in_shape)) {
            message = "fadeInShape must be linear|equalPower";
            return false;
        }
        if (get_string(*it, "fadeOutShape", text) &&
            !parse_fade_shape(text, options.post.fade_out_shape)) {
            message = "fadeOutShape must be linear|equalPower";
            return false;
        }
        if (get_string(*it, "normalize", text) &&
            !parse_normalize_mode(text, options.post.normalize)) {
            message = "normalize must be none|peak|truePeak|lufs";
            return false;
        }
        get_double(*it, "normalizeTargetDb", options.post.normalize_target_db);
    }
    return true;
}

// ── Command classification ───────────────────────────────────────────

// Web ops whose session surface does not exist natively yet (pattern
// and order-list structure editing, channel-count reshape, seq layer
// count management). Known names get a typed "unsupported" refusal —
// distinct from invalidOp so scripts can tell "wrong name" from "not
// here yet".
bool op_is_unsupported(const std::string& op) {
    static constexpr std::array<const char*, 12> kUnsupported = {
        "insertRow",     "deleteRow",     "resizePattern", "createPattern",
        "deletePattern", "renamePattern", "setOrderList",  "insertOrderAt",
        "removeOrderAt", "setChannels",   "addSeqLayer",   "removeSeqLayer",
    };
    return std::any_of(kUnsupported.begin(), kUnsupported.end(),
                       [&op](const char* name) { return op == name; });
}

bool op_is_known(const std::string& op) {
    static constexpr std::array<const char*, 33> kKnown = {
        // Web-parity surface
        "setCell", "clearCell", "setNoteOff", "setRange", "setBpm", "setSpeed", "renameSample",
        "setSampleMeta", "setSampleStretchRatio", "conformSampleToRows", "setInstrumentSlot",
        "clearInstrumentSlot", "addSeqNote", "removeSeqNote", "updateSeqNote",
        "setSeqLayerInstrument", "setSeqLayerEnabled",
        // Native additions
        "play", "stop", "newProject", "loadProject", "saveProject", "exportProject",
        "loadSampleFile", "loadSampleData", "clearSample", "addWorkspaceNode", "addPluginNode",
        "removeWorkspaceNode", "addCable", "removeCable", "setCableMode", "setPluginParam"};
    return std::any_of(kKnown.begin(), kKnown.end(),
                       [&op](const char* name) { return op == name; });
}

// ── Shared validation fragments ──────────────────────────────────────

// patternId → pattern array index, or an error pushed and -1.
int require_pattern(const json& cmd, int index, const engine::TrackerProject& project,
                    std::vector<json>& errors) {
    int pattern_id = 0;
    if (!get_int(cmd, "patternId", pattern_id)) {
        errors.push_back(make_error(index, kErrInvalidField, "patternId must be an integer"));
        return -1;
    }
    const int pattern_index = pattern_index_by_id(project, pattern_id);
    if (pattern_index < 0) {
        errors.push_back(make_error(index, kErrNotFound,
                                    "pattern " + std::to_string(pattern_id) + " not found"));
    }
    return pattern_index;
}

bool require_cell_coords(const json& cmd, int index, const engine::TrackerProject& project,
                         int pattern_index, int& row, int& channel, std::vector<json>& errors) {
    const auto& pattern = project.patterns[static_cast<std::size_t>(pattern_index)];
    if (!get_int(cmd, "row", row) || !get_int(cmd, "channel", channel)) {
        errors.push_back(make_error(index, kErrInvalidField, "row and channel must be integers"));
        return false;
    }
    if (row < 0 || row >= static_cast<int>(pattern.rows.size())) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "row " + std::to_string(row) + " out of range (0.." +
                                        std::to_string(pattern.rows.size() - 1) + ")"));
        return false;
    }
    if (channel < 0 || channel >= project.channels) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "channel " + std::to_string(channel) + " out of range (0.." +
                                        std::to_string(project.channels - 1) + ")"));
        return false;
    }
    return true;
}

// sampleId → sample meta, or an error pushed and nullptr.
const engine::TrackerSample* require_sample(const json& cmd, int index,
                                            const engine::TrackerProject& project,
                                            std::vector<json>& errors) {
    int sample_id = 0;
    if (!get_int(cmd, "sampleId", sample_id)) {
        errors.push_back(make_error(index, kErrInvalidField, "sampleId must be an integer"));
        return nullptr;
    }
    const engine::TrackerSample* sample = sample_by_id(project, sample_id);
    if (sample == nullptr) {
        errors.push_back(
            make_error(index, kErrNotFound, "sample " + std::to_string(sample_id) + " not found"));
    }
    return sample;
}

bool require_slot(const json& cmd, int index, std::vector<json>& errors) {
    int slot = 0;
    if (!get_int(cmd, "slot", slot)) {
        errors.push_back(make_error(index, kErrInvalidField, "slot must be an integer"));
        return false;
    }
    if (slot < 1 || slot > engine::kMaxSamples) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "slot " + std::to_string(slot) + " out of range (1.." +
                                        std::to_string(engine::kMaxSamples) + ")"));
        return false;
    }
    return true;
}

bool require_layer_coords(const json& cmd, int index, const engine::TrackerProject& project,
                          int& channel, int& layer, std::vector<json>& errors) {
    if (!get_int(cmd, "channel", channel) || !get_int(cmd, "layerIndex", layer)) {
        errors.push_back(
            make_error(index, kErrInvalidField, "channel and layerIndex must be integers"));
        return false;
    }
    if (channel < 0 || channel >= project.channels) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "channel " + std::to_string(channel) + " out of range (0.." +
                                        std::to_string(project.channels - 1) + ")"));
        return false;
    }
    if (layer < 0 || layer >= engine::kMaxSeqLayersPerChannel) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "layerIndex " + std::to_string(layer) + " out of range (0.." +
                                        std::to_string(engine::kMaxSeqLayersPerChannel - 1) + ")"));
        return false;
    }
    return true;
}

bool require_note(const json& cmd, int index, const engine::TrackerProject& project,
                  int pattern_index, engine::SequenceNote& note, std::vector<json>& errors) {
    const auto it = cmd.find("note");
    if (it == cmd.end() || !it->is_object() || !note_from_json(*it, note)) {
        errors.push_back(make_error(
            index, kErrInvalidField,
            "note must be an object with pitch/startTick/durationTicks/velocity integers"));
        return false;
    }
    const auto& pattern = project.patterns[static_cast<std::size_t>(pattern_index)];
    const int max_tick = static_cast<int>(pattern.rows.size()) * project.speed;
    if (note.pitch < 0 || note.pitch > 127) {
        errors.push_back(make_error(index, kErrInvalidField, "pitch must be 0..127"));
        return false;
    }
    if (note.start_tick < 0 || note.start_tick >= max_tick) {
        errors.push_back(make_error(index, kErrOutOfBounds,
                                    "startTick " + std::to_string(note.start_tick) +
                                        " out of pattern range (0.." +
                                        std::to_string(max_tick - 1) + ")"));
        return false;
    }
    if (note.duration_ticks < 1) {
        errors.push_back(make_error(index, kErrInvalidField, "durationTicks must be >= 1"));
        return false;
    }
    if (note.velocity < 0 || note.velocity > 127) {
        errors.push_back(make_error(index, kErrInvalidField, "velocity must be 0..127"));
        return false;
    }
    return true;
}

bool require_cable_end(const json& cmd, int index, const char* key, graph::CableEnd& end,
                       std::vector<json>& errors) {
    const auto it = cmd.find(key);
    if (it == cmd.end() || !it->is_object() || !get_string(*it, "nodeId", end.node_id) ||
        !get_string(*it, "portId", end.port_id)) {
        errors.push_back(make_error(index, kErrInvalidField,
                                    std::string(key) + " must be {nodeId, portId} strings"));
        return false;
    }
    return true;
}

bool parse_cable_mode(const json& cmd, int index, graph::CableMode& mode,
                      std::vector<json>& errors) {
    std::string text;
    if (const auto it = cmd.find("mode"); it != cmd.end()) {
        if (!it->is_string()) {
            errors.push_back(make_error(index, kErrInvalidField, "mode must be tap|reroute"));
            return false;
        }
        text = it->get<std::string>();
        if (text == "tap") {
            mode = graph::CableMode::kTap;
        } else if (text == "reroute") {
            mode = graph::CableMode::kReroute;
        } else {
            errors.push_back(make_error(index, kErrInvalidField, "mode must be tap|reroute"));
            return false;
        }
    }
    return true;
}

// Instrument-table entry from JSON (web InstrumentTableEntry shape).
bool parse_instrument_entry(const json& src, engine::InstrumentTableEntry& entry,
                            std::string& message) {
    std::string type;
    if (!get_string(src, "type", type)) {
        message = "entry.type must be sample|plugin|workspace";
        return false;
    }
    if (type == "sample") {
        entry.type = engine::InstrumentSourceType::kSample;
    } else if (type == "plugin") {
        entry.type = engine::InstrumentSourceType::kPlugin;
    } else if (type == "workspace") {
        entry.type = engine::InstrumentSourceType::kWorkspace;
    } else {
        message = "entry.type must be sample|plugin|workspace";
        return false;
    }
    get_int(src, "sampleId", entry.sample_id);
    get_string(src, "pluginId", entry.plugin_id);
    get_string(src, "workspaceId", entry.workspace_id);
    if (const auto it = src.find("pluginPresetParams"); it != src.end()) {
        if (!it->is_object()) {
            message = "entry.pluginPresetParams must be an object of numbers";
            return false;
        }
        for (const auto& [key, value] : it->items()) {
            if (!value.is_number()) {
                message = "entry.pluginPresetParams must be an object of numbers";
                return false;
            }
            entry.plugin_preset_params.emplace_back(key, value.get<double>());
        }
    }
    if (const auto it = src.find("boundTracks"); it != src.end()) {
        if (!it->is_array()) {
            message = "entry.boundTracks must be an array of integers";
            return false;
        }
        for (const auto& value : *it) {
            if (!value.is_number_integer()) {
                message = "entry.boundTracks must be an array of integers";
                return false;
            }
            entry.bound_tracks.push_back(value.get<int>());
        }
        std::sort(entry.bound_tracks.begin(), entry.bound_tracks.end());
        entry.bound_tracks.erase(std::unique(entry.bound_tracks.begin(), entry.bound_tracks.end()),
                                 entry.bound_tracks.end());
    }
    return true;
}

// ── Validation (phase 1 — never mutates) ─────────────────────────────

// NOLINTNEXTLINE(readability-function-size) — one arm per command; the
// tagged-union dispatch mirrors the web executor's validateCommand.
void validate_command(const json& cmd, int index, app::ProjectSession& session,
                      std::vector<json>& errors) {
    const engine::TrackerProject& project = session.project();
    std::string op;
    if (!cmd.is_object() || !get_string(cmd, "op", op)) {
        errors.push_back(make_error(index, kErrInvalidField, "command must be {op, …}"));
        return;
    }
    if (op_is_unsupported(op)) {
        errors.push_back(
            make_error(index, kErrUnsupported, op + " is not supported by the native session yet"));
        return;
    }
    if (!op_is_known(op)) {
        errors.push_back(make_error(index, kErrInvalidOp, "unknown op: " + op));
        return;
    }

    if (op == "setCell" || op == "clearCell" || op == "setNoteOff") {
        const int pattern_index = require_pattern(cmd, index, project, errors);
        if (pattern_index < 0) {
            return;
        }
        int row = 0;
        int channel = 0;
        if (!require_cell_coords(cmd, index, project, pattern_index, row, channel, errors)) {
            return;
        }
        if (op == "setCell") {
            const auto it = cmd.find("cell");
            if (it == cmd.end() || !it->is_object()) {
                errors.push_back(make_error(index, kErrInvalidField, "cell must be an object"));
                return;
            }
            engine::TrackerCell probe;
            std::string bad_field;
            if (!merge_cell_fields(*it, probe, bad_field)) {
                errors.push_back(
                    make_error(index, kErrInvalidField, "cell." + bad_field + " must be a number"));
            }
        }
        return;
    }
    if (op == "setRange") {
        const int pattern_index = require_pattern(cmd, index, project, errors);
        if (pattern_index < 0) {
            return;
        }
        const auto& pattern = project.patterns[static_cast<std::size_t>(pattern_index)];
        int row_start = 0;
        if (!get_int(cmd, "rowStart", row_start) || row_start < 0 ||
            row_start >= static_cast<int>(pattern.rows.size())) {
            errors.push_back(make_error(index, kErrOutOfBounds, "rowStart out of range"));
            return;
        }
        const auto channels_it = cmd.find("channels");
        const auto cells_it = cmd.find("cells");
        if (channels_it == cmd.end() || !channels_it->is_array() || cells_it == cmd.end() ||
            !cells_it->is_array()) {
            errors.push_back(
                make_error(index, kErrInvalidField, "channels and cells must be arrays"));
            return;
        }
        for (const auto& ch : *channels_it) {
            if (!ch.is_number_integer() || ch.get<int>() < 0 || ch.get<int>() >= project.channels) {
                errors.push_back(make_error(index, kErrOutOfBounds, "channel out of range"));
                return;
            }
        }
        if (row_start + static_cast<int>(cells_it->size()) >
            static_cast<int>(pattern.rows.size())) {
            errors.push_back(make_error(index, kErrOutOfBounds, "setRange overruns pattern"));
            return;
        }
        for (const auto& row : *cells_it) {
            if (!row.is_array() || row.size() != channels_it->size()) {
                errors.push_back(make_error(index, kErrInvalidField,
                                            "each cells[row] must match channels.length"));
                return;
            }
            for (const auto& cell : row) {
                engine::TrackerCell probe;
                std::string bad_field;
                if (!cell.is_object() || !merge_cell_fields(cell, probe, bad_field)) {
                    errors.push_back(
                        make_error(index, kErrInvalidField, "cells must be cell objects"));
                    return;
                }
            }
        }
        return;
    }
    if (op == "setBpm") {
        double value = 0.0;
        if (!get_double(cmd, "value", value) || value < 32.0 || value > 999.0) {
            errors.push_back(make_error(index, kErrInvalidField, "bpm must be 32..999"));
        }
        return;
    }
    if (op == "setSpeed") {
        int value = 0;
        if (!get_int(cmd, "value", value) || value < 1 || value > 31) {
            errors.push_back(make_error(index, kErrInvalidField, "speed must be 1..31"));
        }
        return;
    }
    if (op == "renameSample") {
        if (require_sample(cmd, index, project, errors) == nullptr) {
            return;
        }
        std::string name;
        if (!get_string(cmd, "name", name)) {
            errors.push_back(make_error(index, kErrInvalidField, "name must be a string"));
        }
        return;
    }
    if (op == "setSampleMeta") {
        if (require_sample(cmd, index, project, errors) == nullptr) {
            return;
        }
        const auto it = cmd.find("patch");
        if (it == cmd.end() || !it->is_object()) {
            errors.push_back(make_error(index, kErrInvalidField, "patch must be an object"));
        }
        return;
    }
    if (op == "setSampleStretchRatio") {
        if (require_sample(cmd, index, project, errors) == nullptr) {
            return;
        }
        double ratio = 0.0;
        if (!get_double(cmd, "ratio", ratio) || ratio < 0.01 || ratio > 100.0) {
            errors.push_back(make_error(index, kErrInvalidField, "ratio must be 0.01..100"));
        }
        return;
    }
    if (op == "conformSampleToRows") {
        const engine::TrackerSample* sample = require_sample(cmd, index, project, errors);
        if (sample == nullptr) {
            return;
        }
        if (sample->frames == 0 || sample->sample_rate == 0) {
            errors.push_back(make_error(index, kErrInvalidField, "sample has no decoded duration"));
            return;
        }
        int rows = 0;
        if (!get_int(cmd, "rows", rows) || rows < 1 || rows > 4096) {
            errors.push_back(make_error(index, kErrInvalidField, "rows must be 1..4096"));
            return;
        }
        int pattern_id = 0;
        if (get_int(cmd, "patternId", pattern_id) && pattern_index_by_id(project, pattern_id) < 0) {
            errors.push_back(make_error(index, kErrNotFound,
                                        "pattern " + std::to_string(pattern_id) + " not found"));
        }
        return;
    }
    if (op == "setInstrumentSlot") {
        if (!require_slot(cmd, index, errors)) {
            return;
        }
        const auto it = cmd.find("entry");
        if (it == cmd.end() || !it->is_object()) {
            errors.push_back(make_error(index, kErrInvalidField, "entry must be an object"));
            return;
        }
        engine::InstrumentTableEntry entry;
        std::string message;
        if (!parse_instrument_entry(*it, entry, message)) {
            errors.push_back(make_error(index, kErrInvalidField, message));
            return;
        }
        // Fix-list #5: bogus references are typed errors, never stored
        // silently (the web stored any workspaceId and rendered
        // "(missing)").
        if (entry.type == engine::InstrumentSourceType::kWorkspace) {
            const graph::Node* node = session.workspace().find_node(entry.workspace_id);
            if (node == nullptr || node->kind != graph::NodeKind::kPlugin) {
                errors.push_back(make_error(index, kErrNotFound,
                                            "workspace node '" + entry.workspace_id +
                                                "' is not a live plugin instance"));
            }
        } else if (entry.type == engine::InstrumentSourceType::kPlugin) {
            if (session.plugin_registry().find(entry.plugin_id) == nullptr) {
                errors.push_back(
                    make_error(index, kErrNotFound,
                               "plugin '" + entry.plugin_id + "' is not in the catalogue"));
            }
        }
        return;
    }
    if (op == "clearInstrumentSlot") {
        int slot = 0;
        if (!get_int(cmd, "slot", slot)) {
            errors.push_back(make_error(index, kErrInvalidField, "slot must be an integer"));
            return;
        }
        if (slot < 1 || slot > static_cast<int>(project.instrument_table.size())) {
            errors.push_back(make_error(index, kErrOutOfBounds, "slot out of range"));
        }
        return;
    }
    if (op == "addSeqNote" || op == "removeSeqNote" || op == "updateSeqNote" ||
        op == "setSeqLayerInstrument" || op == "setSeqLayerEnabled") {
        const int pattern_index = require_pattern(cmd, index, project, errors);
        if (pattern_index < 0) {
            return;
        }
        int channel = 0;
        int layer = 0;
        if (!require_layer_coords(cmd, index, project, channel, layer, errors)) {
            return;
        }
        if (op == "addSeqNote" || op == "updateSeqNote") {
            engine::SequenceNote note;
            if (!require_note(cmd, index, project, pattern_index, note, errors)) {
                return;
            }
        }
        if (op == "removeSeqNote" || op == "updateSeqNote") {
            const engine::SequenceLayer* target =
                peek_seq_layer(project, pattern_index, channel, layer);
            int note_index = 0;
            if (!get_int(cmd, "noteIndex", note_index)) {
                errors.push_back(
                    make_error(index, kErrInvalidField, "noteIndex must be an integer"));
                return;
            }
            if (target == nullptr || note_index < 0 ||
                note_index >= static_cast<int>(target->notes.size())) {
                errors.push_back(make_error(index, kErrOutOfBounds, "noteIndex out of range"));
                return;
            }
        }
        if (op == "setSeqLayerInstrument") {
            int instrument = 0;
            if (!get_int(cmd, "instrument", instrument) || instrument < 0 || instrument > 255) {
                errors.push_back(make_error(index, kErrInvalidField, "instrument must be 0..255"));
            }
        }
        if (op == "setSeqLayerEnabled") {
            bool enabled = false;
            if (!get_bool(cmd, "enabled", enabled)) {
                errors.push_back(make_error(index, kErrInvalidField, "enabled must be a boolean"));
                return;
            }
            // Enabled toggles never create layers (session contract),
            // so the layer must already exist.
            if (peek_seq_layer(project, pattern_index, channel, layer) == nullptr) {
                errors.push_back(
                    make_error(index, kErrNotFound, "layer does not exist yet (add a note first)"));
            }
        }
        return;
    }
    if (op == "play" || op == "stop" || op == "newProject") {
        return; // no arguments
    }
    if (op == "loadProject" || op == "saveProject") {
        std::string path;
        if (!get_string(cmd, "path", path) || path.empty()) {
            errors.push_back(
                make_error(index, kErrInvalidField, "path must be a non-empty string"));
        }
        return;
    }
    if (op == "exportProject") {
        std::string path;
        if (!get_string(cmd, "path", path) || path.empty()) {
            errors.push_back(
                make_error(index, kErrInvalidField, "path must be a non-empty string"));
            return;
        }
        io::ExportOptions options;
        std::string message;
        if (!parse_export_options(cmd, options, message)) {
            errors.push_back(make_error(index, kErrInvalidField, message));
        }
        return;
    }
    if (op == "loadSampleFile") {
        if (!require_slot(cmd, index, errors)) {
            return;
        }
        std::string path;
        if (!get_string(cmd, "path", path) || path.empty()) {
            errors.push_back(
                make_error(index, kErrInvalidField, "path must be a non-empty string"));
        }
        return;
    }
    if (op == "loadSampleData") {
        if (!require_slot(cmd, index, errors)) {
            return;
        }
        std::string data;
        if (!get_string(cmd, "dataBase64", data) || data.empty()) {
            errors.push_back(make_error(index, kErrInvalidField,
                                        "dataBase64 must be a non-empty base64 string"));
        }
        return;
    }
    if (op == "clearSample") {
        require_slot(cmd, index, errors);
        return;
    }
    if (op == "addWorkspaceNode") {
        std::string kind;
        if (!get_string(cmd, "kind", kind) ||
            (kind != "sum" && kind != "extMidiIn" && kind != "extMidiOut")) {
            errors.push_back(
                make_error(index, kErrInvalidField, "kind must be sum|extMidiIn|extMidiOut"));
        }
        return;
    }
    if (op == "addPluginNode") {
        std::string plugin_id;
        if (!get_string(cmd, "pluginId", plugin_id)) {
            errors.push_back(make_error(index, kErrInvalidField, "pluginId must be a string"));
            return;
        }
        if (session.plugin_registry().find(plugin_id) == nullptr) {
            errors.push_back(make_error(index, kErrNotFound,
                                        "plugin '" + plugin_id + "' is not in the catalogue"));
        }
        return;
    }
    if (op == "removeWorkspaceNode") {
        std::string workspace_id;
        if (!get_string(cmd, "workspaceId", workspace_id)) {
            errors.push_back(make_error(index, kErrInvalidField, "workspaceId must be a string"));
            return;
        }
        if (session.workspace().find_node(workspace_id) == nullptr) {
            errors.push_back(
                make_error(index, kErrNotFound, "workspace node '" + workspace_id + "' not found"));
            return;
        }
        if (workspace_id == graph::kTrackerBusId || workspace_id == graph::kMasterInId ||
            workspace_id == graph::kModulePlayerId) {
            errors.push_back(
                make_error(index, kErrInvalidField, "built-in nodes cannot be removed"));
        }
        return;
    }
    if (op == "addCable") {
        graph::CableEnd source;
        graph::CableEnd dest;
        graph::CableMode mode = graph::CableMode::kTap;
        if (!require_cable_end(cmd, index, "source", source, errors) ||
            !require_cable_end(cmd, index, "dest", dest, errors) ||
            !parse_cable_mode(cmd, index, mode, errors)) {
            return;
        }
        // Pre-validate with the same checks connect() applies so the
        // whole batch can be rejected before any mutation.
        const graph::Node* src_node = session.workspace().find_node(source.node_id);
        if (src_node == nullptr) {
            errors.push_back(
                make_error(index, kErrNotFound, "source node '" + source.node_id + "' not found"));
            return;
        }
        const graph::Node* dst_node = session.workspace().find_node(dest.node_id);
        if (dst_node == nullptr) {
            errors.push_back(
                make_error(index, kErrNotFound, "dest node '" + dest.node_id + "' not found"));
            return;
        }
        const graph::Port* src_port = graph::find_output(*src_node, source.port_id);
        if (src_port == nullptr) {
            errors.push_back(make_error(index, kErrNotFound,
                                        "source port '" + source.port_id + "' not found on '" +
                                            source.node_id + "'"));
            return;
        }
        const graph::Port* dst_port = graph::find_input(*dst_node, dest.port_id);
        if (dst_port == nullptr) {
            errors.push_back(
                make_error(index, kErrNotFound,
                           "dest port '" + dest.port_id + "' not found on '" + dest.node_id + "'"));
            return;
        }
        if (!graph::port_kinds_compatible(src_port->kind, dst_port->kind)) {
            errors.push_back(make_error(index, kErrInvalidField,
                                        std::string("incompatible port kinds: ") +
                                            graph::port_kind_name(src_port->kind) + " → " +
                                            graph::port_kind_name(dst_port->kind)));
        }
        return;
    }
    if (op == "removeCable" || op == "setCableMode") {
        std::string cable_id;
        if (!get_string(cmd, "cableId", cable_id)) {
            errors.push_back(make_error(index, kErrInvalidField, "cableId must be a string"));
            return;
        }
        if (session.workspace().find_cable(cable_id) == nullptr) {
            errors.push_back(make_error(index, kErrNotFound, "cable '" + cable_id + "' not found"));
            return;
        }
        if (op == "setCableMode") {
            graph::CableMode mode = graph::CableMode::kTap;
            if (cmd.find("mode") == cmd.end()) {
                errors.push_back(make_error(index, kErrInvalidField, "mode must be tap|reroute"));
                return;
            }
            parse_cable_mode(cmd, index, mode, errors);
        }
        return;
    }
    if (op == "setPluginParam") {
        std::string workspace_id;
        if (!get_string(cmd, "workspaceId", workspace_id)) {
            errors.push_back(make_error(index, kErrInvalidField, "workspaceId must be a string"));
            return;
        }
        double value = 0.0;
        if (!get_double(cmd, "value", value)) {
            errors.push_back(make_error(index, kErrInvalidField, "value must be a number"));
            return;
        }
        if (plugins::NtpInstance* ntp = session.plugin_instance(workspace_id)) {
            std::string key;
            if (!get_string(cmd, "key", key) || key.empty()) {
                errors.push_back(make_error(index, kErrInvalidField,
                                            "key must be a non-empty string for NTP plugins"));
                return;
            }
            // Manifest host params or dot-path node params are both
            // legal set_param targets; anything else is a typo.
            const auto& params = ntp->manifest().params;
            const bool known = key.find('.') != std::string::npos ||
                               std::any_of(params.begin(), params.end(),
                                           [&key](const ntp::ParamDef& p) { return p.key == key; });
            if (!known) {
                errors.push_back(
                    make_error(index, kErrNotFound,
                               "param '" + key + "' not found on '" + workspace_id + "'"));
            }
            return;
        }
        if (ext::ClapPlugin* clap = session.clap_instance(workspace_id)) {
            int param_id = 0;
            if (!get_int(cmd, "key", param_id)) {
                errors.push_back(make_error(index, kErrInvalidField,
                                            "key must be a numeric param id for CLAP plugins"));
                return;
            }
            const auto& params = clap->params();
            const bool known =
                std::any_of(params.begin(), params.end(), [param_id](const ext::ClapParamInfo& p) {
                    return p.id == static_cast<clap_id>(param_id);
                });
            if (!known) {
                errors.push_back(make_error(index, kErrNotFound,
                                            "param " + std::to_string(param_id) +
                                                " not found on '" + workspace_id + "'"));
            }
            return;
        }
        if (ext::Vst3Plugin* vst3 = session.vst3_instance(workspace_id)) {
            int param_id = 0;
            if (!get_int(cmd, "key", param_id)) {
                errors.push_back(make_error(index, kErrInvalidField,
                                            "key must be a numeric param id for VST3 plugins"));
                return;
            }
            const auto& params = vst3->params();
            const bool known =
                std::any_of(params.begin(), params.end(), [param_id](const ext::Vst3ParamInfo& p) {
                    return p.id == static_cast<std::uint32_t>(param_id);
                });
            if (!known) {
                errors.push_back(make_error(index, kErrNotFound,
                                            "param " + std::to_string(param_id) +
                                                " not found on '" + workspace_id + "'"));
                return;
            }
            if (value < 0.0 || value > 1.0) {
                errors.push_back(
                    make_error(index, kErrInvalidField, "VST3 param values are normalized 0..1"));
            }
            return;
        }
        errors.push_back(
            make_error(index, kErrNotFound,
                       "workspace node '" + workspace_id + "' is not a live plugin instance"));
        return;
    }
}

// ── Application (phase 2 — validated commands) ───────────────────────

// Side results a batch accumulates (additive over the web BatchResult).
struct BatchExtras {
    json created_node_ids = json::array();
    json created_cable_ids = json::array();
    json loaded_samples = json::array();
    json export_result; // null until an exportProject lands
};

json sample_summary(const engine::TrackerSample& meta) {
    const double duration =
        meta.sample_rate != 0 ? static_cast<double>(meta.frames) / meta.sample_rate : 0.0;
    return json{{"id", meta.id},
                {"name", meta.name},
                {"sampleRate", meta.sample_rate},
                {"numChannels", meta.num_channels},
                {"frames", meta.frames},
                {"durationSeconds", duration}};
}

// Loads decoded upload bytes through the standard slot-load path. The
// session API is file-based, so the bytes take a temp-file round trip;
// the decode itself is the same load_sample_memory sniff the file path
// uses.
bool load_sample_bytes(app::ProjectSession& session, int slot,
                       const std::vector<std::uint8_t>& bytes, const std::string& name,
                       std::string& message) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path temp_root = fs::temp_directory_path(ec);
    if (ec) {
        message = "cannot resolve temp directory";
        return false;
    }
    const fs::path dir =
        temp_root / ("nt-api-upload-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directories(dir, ec) || ec) {
        message = "cannot create upload temp directory";
        return false;
    }
    const fs::path file_path = dir / sanitize_upload_name(name);
    {
        std::ofstream file(file_path, std::ios::binary);
        if (!file) {
            message = "cannot write upload temp file";
            fs::remove_all(dir, ec);
            return false;
        }
        file.write(
            reinterpret_cast<const char*>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            message = "cannot write upload temp file";
            fs::remove_all(dir, ec);
            return false;
        }
    }
    const bool ok = session.load_sample_into_slot(slot, file_path);
    if (!ok) {
        message = session.error();
    }
    fs::remove_all(dir, ec);
    return ok;
}

// Commands validate against pre-batch state (web semantics), but a
// batch can contain project-replacing ops (loadProject, newProject,
// clearSample) before commands that were validated against the old
// state — apply-side reads therefore re-check every lookup before
// dereferencing and fail typed instead of trusting phase 1.
json stale_error(int index) {
    return make_error(index, kErrOutOfBounds,
                      "target invalidated by an earlier command in this batch");
}

// Bounds re-check for apply-side cell reads.
bool cell_in_range(const engine::TrackerProject& project, int pattern_index, int row, int channel) {
    if (pattern_index < 0 || pattern_index >= static_cast<int>(project.patterns.size())) {
        return false;
    }
    const auto& rows = project.patterns[static_cast<std::size_t>(pattern_index)].rows;
    return row >= 0 && row < static_cast<int>(rows.size()) && channel >= 0 &&
           channel < static_cast<int>(rows[static_cast<std::size_t>(row)].size());
}

// Applies one validated command. Returns a null json on success or an
// error object (I/O and decode failures surface here).
// NOLINTNEXTLINE(readability-function-size) — one arm per command.
json apply_command(const json& cmd, int index, app::ProjectSession& session, BatchExtras& extras) {
    engine::TrackerProject& project = session.project();
    std::string op;
    get_string(cmd, "op", op);

    if (op == "setCell" || op == "clearCell" || op == "setNoteOff") {
        int pattern_id = 0;
        get_int(cmd, "patternId", pattern_id);
        const int pattern_index = pattern_index_by_id(project, pattern_id);
        int row = 0;
        int channel = 0;
        get_int(cmd, "row", row);
        get_int(cmd, "channel", channel);
        if (!cell_in_range(project, pattern_index, row, channel)) {
            return stale_error(index);
        }
        engine::TrackerCell cell; // empty cell = clearCell result
        if (op == "setCell") {
            cell = project.patterns[static_cast<std::size_t>(pattern_index)]
                       .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)];
            std::string bad_field;
            merge_cell_fields(cmd.at("cell"), cell, bad_field);
        } else if (op == "setNoteOff") {
            cell.note = engine::kNoteOff; // canonical "==" cell shape
        }
        session.set_cell(pattern_index, row, channel, cell);
        return {};
    }
    if (op == "setRange") {
        int pattern_id = 0;
        int row_start = 0;
        get_int(cmd, "patternId", pattern_id);
        get_int(cmd, "rowStart", row_start);
        const int pattern_index = pattern_index_by_id(project, pattern_id);
        const auto& channels = cmd.at("channels");
        const auto& cells = cmd.at("cells");
        for (std::size_t r = 0; r < cells.size(); ++r) {
            for (std::size_t c = 0; c < channels.size(); ++c) {
                const int row = row_start + static_cast<int>(r);
                const int channel = channels[c].get<int>();
                if (!cell_in_range(project, pattern_index, row, channel)) {
                    return stale_error(index);
                }
                engine::TrackerCell cell =
                    project.patterns[static_cast<std::size_t>(pattern_index)]
                        .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)];
                std::string bad_field;
                merge_cell_fields(cells[r][c], cell, bad_field);
                session.set_cell(pattern_index, row, channel, cell);
            }
        }
        return {};
    }
    if (op == "setBpm" || op == "setSpeed") {
        double value = 0.0;
        get_double(cmd, "value", value);
        // Scalar project fields are the same concurrency class as cell
        // fields (fixed-size writes against the audio thread's reader —
        // see the session threading contract), so this writes directly
        // and records the inverse for undo.
        engine::TrackerProject* target = &project;
        if (op == "setBpm") {
            const int before = target->bpm;
            const int after = static_cast<int>(std::lround(value));
            target->bpm = after;
            session.undo().push(
                "api: bpm", [target, before] { target->bpm = before; },
                [target, after] { target->bpm = after; });
        } else {
            const int before = target->speed;
            const int after = static_cast<int>(std::lround(value));
            target->speed = after;
            session.undo().push(
                "api: speed", [target, before] { target->speed = before; },
                [target, after] { target->speed = after; });
        }
        return {};
    }
    if (op == "renameSample" || op == "setSampleMeta" || op == "setSampleStretchRatio" ||
        op == "conformSampleToRows") {
        int sample_id = 0;
        get_int(cmd, "sampleId", sample_id);
        const engine::TrackerSample* current = sample_by_id(project, sample_id);
        if (current == nullptr) {
            return stale_error(index);
        }
        engine::TrackerSample meta = *current;
        if (op == "renameSample") {
            get_string(cmd, "name", meta.name);
        } else if (op == "setSampleMeta") {
            const json& patch = cmd.at("patch");
            get_string(patch, "name", meta.name);
            get_int(patch, "baseNote", meta.base_note);
            get_int(patch, "finetune", meta.finetune);
            get_int(patch, "volume", meta.volume);
            get_int(patch, "pan", meta.pan);
            int loop = 0;
            if (get_int(patch, "loopStart", loop) && loop >= 0) {
                meta.loop_start = static_cast<std::uint32_t>(loop);
            }
            if (get_int(patch, "loopLength", loop) && loop >= 0) {
                meta.loop_length = static_cast<std::uint32_t>(loop);
            }
            get_double(patch, "stretchRatio", meta.stretch_ratio);
            get_int(patch, "category", meta.category);
        } else if (op == "setSampleStretchRatio") {
            get_double(cmd, "ratio", meta.stretch_ratio);
        } else { // conformSampleToRows — IT timing, web formula verbatim
            int rows = 0;
            get_int(cmd, "rows", rows);
            const double natural_seconds = static_cast<double>(meta.frames) / meta.sample_rate;
            const double target_seconds =
                (static_cast<double>(rows) * project.speed * 5.0) / (project.bpm * 2.0);
            meta.stretch_ratio = std::clamp(natural_seconds / target_seconds, 0.01, 100.0);
        }
        session.set_sample_meta(sample_id, meta);
        return {};
    }
    if (op == "setInstrumentSlot" || op == "clearInstrumentSlot") {
        int slot = 0;
        get_int(cmd, "slot", slot);
        engine::InstrumentTableEntry entry;
        if (op == "setInstrumentSlot") {
            std::string message;
            parse_instrument_entry(cmd.at("entry"), entry, message);
        } else {
            entry.sample_id = slot; // default sample entry (web shape)
        }
        session.set_instrument_entry(slot, entry);
        return {};
    }
    if (op == "addSeqNote" || op == "removeSeqNote" || op == "updateSeqNote" ||
        op == "setSeqLayerInstrument" || op == "setSeqLayerEnabled") {
        int pattern_id = 0;
        int channel = 0;
        int layer = 0;
        get_int(cmd, "patternId", pattern_id);
        get_int(cmd, "channel", channel);
        get_int(cmd, "layerIndex", layer);
        const int pattern_index = pattern_index_by_id(project, pattern_id);
        if (op == "addSeqNote") {
            engine::SequenceNote note;
            note_from_json(cmd.at("note"), note);
            session.seq_add_note(pattern_index, channel, layer, note);
        } else if (op == "removeSeqNote") {
            int note_index = 0;
            get_int(cmd, "noteIndex", note_index);
            session.seq_remove_note(pattern_index, channel, layer, note_index);
        } else if (op == "updateSeqNote") {
            int note_index = 0;
            get_int(cmd, "noteIndex", note_index);
            engine::SequenceNote note;
            note_from_json(cmd.at("note"), note);
            const engine::SequenceLayer* target =
                peek_seq_layer(project, pattern_index, channel, layer);
            if (target == nullptr || note_index < 0 ||
                note_index >= static_cast<int>(target->notes.size())) {
                return stale_error(index);
            }
            std::vector<engine::SequenceNote> notes = target->notes;
            notes[static_cast<std::size_t>(note_index)] = note;
            session.seq_replace_notes(pattern_index, channel, layer, std::move(notes));
        } else if (op == "setSeqLayerInstrument") {
            int instrument = 0;
            get_int(cmd, "instrument", instrument);
            session.seq_set_layer_instrument(pattern_index, channel, layer, instrument);
        } else {
            bool enabled = false;
            get_bool(cmd, "enabled", enabled);
            session.seq_set_layer_enabled(pattern_index, channel, layer, enabled);
        }
        return {};
    }
    if (op == "play") {
        session.play();
        return {};
    }
    if (op == "stop") {
        session.stop();
        return {};
    }
    if (op == "newProject") {
        session.new_project();
        return {};
    }
    if (op == "loadProject") {
        std::string path;
        get_string(cmd, "path", path);
        if (!session.load_file(path)) {
            return make_error(index, kErrIoError, session.error());
        }
        return {};
    }
    if (op == "saveProject") {
        std::string path;
        get_string(cmd, "path", path);
        if (!session.save_ftrk(path)) {
            return make_error(index, kErrIoError, session.error());
        }
        return {};
    }
    if (op == "exportProject") {
        std::string path;
        get_string(cmd, "path", path);
        io::ExportOptions options;
        std::string message;
        parse_export_options(cmd, options, message);
        const io::ExportResult result = session.export_current(path, options);
        if (!result.ok) {
            return make_error(index, kErrIoError, result.error);
        }
        json files = json::array();
        for (const std::filesystem::path& file : result.files) {
            files.push_back(file.string());
        }
        extras.export_result = json{
            {"frames", result.frames}, {"seconds", result.seconds}, {"files", std::move(files)}};
        return {};
    }
    if (op == "loadSampleFile") {
        int slot = 0;
        std::string path;
        get_int(cmd, "slot", slot);
        get_string(cmd, "path", path);
        if (!session.load_sample_into_slot(slot, path)) {
            return make_error(index, kErrIoError, session.error());
        }
        json loaded = sample_summary(*sample_by_id(project, slot));
        extras.loaded_samples.push_back(
            json{{"slot", slot},
                 {"fileName", std::filesystem::path(path).filename().string()},
                 {"sample", std::move(loaded)}});
        return {};
    }
    if (op == "loadSampleData") {
        int slot = 0;
        std::string data;
        std::string name = "upload.wav";
        get_int(cmd, "slot", slot);
        get_string(cmd, "dataBase64", data);
        get_string(cmd, "name", name);
        std::vector<std::uint8_t> bytes;
        std::string message;
        if (!base64_decode(data, bytes, message)) {
            return make_error(index, kErrInvalidField, message);
        }
        if (!load_sample_bytes(session, slot, bytes, name, message)) {
            return make_error(index, kErrIoError, message);
        }
        json loaded = sample_summary(*sample_by_id(project, slot));
        extras.loaded_samples.push_back(
            json{{"slot", slot}, {"fileName", name}, {"sample", std::move(loaded)}});
        return {};
    }
    if (op == "clearSample") {
        int slot = 0;
        get_int(cmd, "slot", slot);
        session.clear_slot(slot);
        return {};
    }
    if (op == "addWorkspaceNode") {
        std::string kind;
        get_string(cmd, "kind", kind);
        std::string node_id;
        if (kind == "sum") {
            node_id = session.add_sum_node();
        } else if (kind == "extMidiIn") {
            node_id = session.add_ext_midi_in_node();
        } else {
            node_id = session.add_ext_midi_out_node();
        }
        extras.created_node_ids.push_back(node_id);
        return {};
    }
    if (op == "addPluginNode") {
        std::string plugin_id;
        get_string(cmd, "pluginId", plugin_id);
        const std::string node_id = session.add_plugin_node(plugin_id);
        if (node_id.empty()) {
            return make_error(index, kErrNotFound,
                              "plugin '" + plugin_id + "' is not in the catalogue");
        }
        extras.created_node_ids.push_back(node_id);
        return {};
    }
    if (op == "removeWorkspaceNode") {
        std::string workspace_id;
        get_string(cmd, "workspaceId", workspace_id);
        session.remove_workspace_node(workspace_id);
        return {};
    }
    if (op == "addCable") {
        graph::CableEnd source;
        graph::CableEnd dest;
        graph::CableMode mode = graph::CableMode::kTap;
        std::vector<json> scratch;
        require_cable_end(cmd, index, "source", source, scratch);
        require_cable_end(cmd, index, "dest", dest, scratch);
        parse_cable_mode(cmd, index, mode, scratch);
        const graph::ConnectResult result = session.add_cable(source, dest, mode);
        if (result != graph::ConnectResult::kOk) {
            return make_error(index, kErrInvalidField, graph::connect_result_message(result));
        }
        extras.created_cable_ids.push_back(session.workspace().cables().back().id);
        return {};
    }
    if (op == "removeCable") {
        std::string cable_id;
        get_string(cmd, "cableId", cable_id);
        session.remove_cable(cable_id);
        return {};
    }
    if (op == "setCableMode") {
        std::string cable_id;
        get_string(cmd, "cableId", cable_id);
        graph::CableMode mode = graph::CableMode::kTap;
        std::vector<json> scratch;
        parse_cable_mode(cmd, index, mode, scratch);
        session.set_cable_mode(cable_id, mode);
        return {};
    }
    if (op == "setPluginParam") {
        std::string workspace_id;
        get_string(cmd, "workspaceId", workspace_id);
        double value = 0.0;
        get_double(cmd, "value", value);
        if (session.plugin_instance(workspace_id) != nullptr) {
            std::string key;
            get_string(cmd, "key", key);
            session.set_plugin_param(workspace_id, key, static_cast<float>(value));
        } else if (ext::ClapPlugin* clap = session.clap_instance(workspace_id)) {
            int param_id = 0;
            get_int(cmd, "key", param_id);
            clap->set_param(static_cast<clap_id>(param_id), value);
        } else if (ext::Vst3Plugin* vst3 = session.vst3_instance(workspace_id)) {
            int param_id = 0;
            get_int(cmd, "key", param_id);
            vst3->set_param(static_cast<std::uint32_t>(param_id), value);
        }
        return {};
    }
    return make_error(index, kErrInvalidOp, "unknown op: " + op); // unreachable after validation
}

// ── Execute (batch) ──────────────────────────────────────────────────

json execute_batch(app::ProjectSession& session, const json& commands, const json& opts) {
    if (!commands.is_array()) {
        return failure(make_error(-1, kErrInvalidField, "commands must be an array"));
    }
    if (commands.size() > static_cast<std::size_t>(kMaxCommandsPerBatch)) {
        return failure(
            make_error(-1, kErrLimitExceeded,
                       "max " + std::to_string(kMaxCommandsPerBatch) + " commands per batch"));
    }
    bool dry_run = false;
    std::string undo_description;
    if (opts.is_object()) {
        get_bool(opts, "dryRun", dry_run);
        get_string(opts, "undoDescription", undo_description);
    }
    if (!dry_run && undo_description.empty()) {
        return failure(make_error(-1, kErrMissingUndoDescription,
                                  "opts.undoDescription is required unless dryRun"));
    }

    // Phase 1: validate everything against pre-batch state (web
    // semantics — intra-batch references are unsupported).
    std::vector<json> errors;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        validate_command(commands[i], static_cast<int>(i), session, errors);
    }
    if (!errors.empty()) {
        return json{{"ok", false}, {"errors", errors}};
    }
    if (dry_run) {
        return json{{"ok", true},
                    {"commandsApplied", commands.size()},
                    {"createdPatternIds", json::array()},
                    {"dryRun", true}};
    }

    // Phase 2: apply as one undo group (structural session ops clear
    // history mid-batch exactly as their UI equivalents do; the group
    // then dissolves harmlessly).
    BatchExtras extras;
    std::size_t applied = 0;
    json apply_error;
    {
        const app::UndoGroup group(session.undo(), "api: " + undo_description);
        for (std::size_t i = 0; i < commands.size(); ++i) {
            apply_error = apply_command(commands[i], static_cast<int>(i), session, extras);
            if (!apply_error.is_null()) {
                break;
            }
            ++applied;
        }
    }
    if (!apply_error.is_null()) {
        // I/O failure mid-batch: earlier commands stay applied (a case
        // the two-phase web surface never had — reported honestly).
        json result = failure(std::move(apply_error));
        result["appliedBeforeFailure"] = applied;
        return result;
    }

    json result{{"ok", true}, {"commandsApplied", applied}, {"createdPatternIds", json::array()}};
    if (!extras.created_node_ids.empty()) {
        result["createdNodeIds"] = std::move(extras.created_node_ids);
    }
    if (!extras.created_cable_ids.empty()) {
        result["createdCableIds"] = std::move(extras.created_cable_ids);
    }
    if (!extras.loaded_samples.empty()) {
        result["loadedSamples"] = std::move(extras.loaded_samples);
    }
    if (!extras.export_result.is_null()) {
        result["exportResult"] = std::move(extras.export_result);
    }
    return result;
}

// ── Queries ──────────────────────────────────────────────────────────

json query_success(json data) {
    return json{{"ok", true}, {"data", std::move(data)}};
}

json project_summary(const engine::TrackerProject& project) {
    return json{{"name", project.name},
                {"bpm", project.bpm},
                {"speed", project.speed},
                {"channels", project.channels},
                {"rowsPerPattern", project.rows_per_pattern},
                {"patternCount", project.patterns.size()},
                {"orderListLength", project.order_list.size()},
                {"sampleCount", project.samples.size()},
                {"instrumentTableSize", project.instrument_table.size()}};
}

json build_schema();

// NOLINTNEXTLINE(readability-function-size) — one arm per query.
json run_query(app::ProjectSession& session, audio::AudioEngine& audio, const json& query) {
    if (!query.is_object()) {
        return failure(make_error(-1, kErrInvalidField, "query must be an object"));
    }
    std::string op;
    if (!get_string(query, "op", op)) {
        return failure(make_error(-1, kErrInvalidField, "query must be {op, …}"));
    }
    const engine::TrackerProject& project = session.project();

    if (op == "getProjectSummary") {
        return query_success(project_summary(project));
    }
    if (op == "getPatternList") {
        json list = json::array();
        for (const engine::TrackerPattern& pattern : project.patterns) {
            int filled = 0;
            for (const auto& row : pattern.rows) {
                filled += static_cast<int>(
                    std::count_if(row.begin(), row.end(),
                                  [](const engine::TrackerCell& c) { return c.note != 0; }));
            }
            json positions = json::array();
            for (std::size_t i = 0; i < project.order_list.size(); ++i) {
                if (project.order_list[i] == pattern.id) {
                    positions.push_back(i);
                }
            }
            list.push_back(json{{"id", pattern.id},
                                {"name", pattern.name},
                                {"rows", pattern.rows.size()},
                                {"filledCells", filled},
                                {"orderListPositions", std::move(positions)}});
        }
        return query_success(std::move(list));
    }
    if (op == "getPattern" || op == "getRange" || op == "getSeqLayer" || op == "getSeqLayerList") {
        std::vector<json> errors;
        const int pattern_index = require_pattern(query, -1, project, errors);
        if (pattern_index < 0) {
            return json{{"ok", false}, {"errors", errors}};
        }
        const auto& pattern = project.patterns[static_cast<std::size_t>(pattern_index)];
        if (op == "getPattern") {
            json rows = json::array();
            for (const auto& row : pattern.rows) {
                json cells = json::array();
                for (const engine::TrackerCell& cell : row) {
                    cells.push_back(cell_to_json(cell));
                }
                rows.push_back(std::move(cells));
            }
            return query_success(
                json{{"id", pattern.id}, {"name", pattern.name}, {"rows", std::move(rows)}});
        }
        if (op == "getRange") {
            int row_start = 0;
            int row_end = 0;
            if (!get_int(query, "rowStart", row_start) || !get_int(query, "rowEnd", row_end) ||
                row_start < 0 || row_end <= row_start ||
                row_end > static_cast<int>(pattern.rows.size())) {
                return failure(make_error(-1, kErrOutOfBounds,
                                          "rowStart/rowEnd out of range (rowEnd exclusive)"));
            }
            std::vector<int> channels;
            if (const auto it = query.find("channels"); it != query.end()) {
                if (!it->is_array()) {
                    return failure(make_error(-1, kErrInvalidField, "channels must be an array"));
                }
                for (const auto& ch : *it) {
                    if (!ch.is_number_integer() || ch.get<int>() < 0 ||
                        ch.get<int>() >= project.channels) {
                        return failure(make_error(-1, kErrOutOfBounds, "channel out of range"));
                    }
                    channels.push_back(ch.get<int>());
                }
            } else {
                for (int ch = 0; ch < project.channels; ++ch) {
                    channels.push_back(ch);
                }
            }
            json rows = json::array();
            for (int row = row_start; row < row_end; ++row) {
                json cells = json::array();
                for (const int ch : channels) {
                    cells.push_back(cell_to_json(
                        pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(ch)]));
                }
                rows.push_back(std::move(cells));
            }
            return query_success(json{{"patternId", pattern.id},
                                      {"rowStart", row_start},
                                      {"rowEnd", row_end},
                                      {"channels", channels},
                                      {"cells", std::move(rows)}});
        }
        if (op == "getSeqLayer") {
            int channel = 0;
            int layer = 0;
            std::vector<json> layer_errors;
            if (!require_layer_coords(query, -1, project, channel, layer, layer_errors)) {
                return json{{"ok", false}, {"errors", layer_errors}};
            }
            const engine::SequenceLayer* target =
                peek_seq_layer(project, pattern_index, channel, layer);
            if (target == nullptr) {
                return failure(
                    make_error(-1, kErrNotFound, "layer does not exist yet (add a note first)"));
            }
            json notes = json::array();
            for (const engine::SequenceNote& note : target->notes) {
                notes.push_back(note_to_json(note));
            }
            return query_success(json{{"patternId", pattern.id},
                                      {"channel", channel},
                                      {"layerIndex", layer},
                                      {"instrument", target->instrument},
                                      {"enabled", target->enabled},
                                      {"notes", std::move(notes)}});
        }
        // getSeqLayerList
        json list = json::array();
        const auto& seq = project.sequence_mixer.seq_patterns;
        if (pattern_index < static_cast<int>(seq.size())) {
            const auto& layers = seq[static_cast<std::size_t>(pattern_index)].layers;
            for (std::size_t ch = 0; ch < layers.size(); ++ch) {
                for (std::size_t layer = 0; layer < layers[ch].size(); ++layer) {
                    const engine::SequenceLayer& entry = layers[ch][layer];
                    list.push_back(json{{"channel", ch},
                                        {"layerIndex", layer},
                                        {"instrument", entry.instrument},
                                        {"enabled", entry.enabled},
                                        {"noteCount", entry.notes.size()}});
                }
            }
        }
        return query_success(std::move(list));
    }
    if (op == "getOrderList") {
        return query_success(json(project.order_list));
    }
    if (op == "getSamples") {
        json list = json::array();
        for (const engine::TrackerSample& sample : project.samples) {
            list.push_back(json{{"id", sample.id},
                                {"name", sample.name},
                                {"fileName", sample.file_name},
                                {"format", sample.format},
                                {"sampleRate", sample.sample_rate},
                                {"numChannels", sample.num_channels},
                                {"frames", sample.frames},
                                {"loopStart", sample.loop_start},
                                {"loopLength", sample.loop_length},
                                {"baseNote", sample.base_note},
                                {"finetune", sample.finetune},
                                {"volume", sample.volume},
                                {"pan", sample.pan},
                                {"stretchRatio", sample.stretch_ratio},
                                {"category", sample.category}});
        }
        return query_success(std::move(list));
    }
    if (op == "getInstrumentTable") {
        json list = json::array();
        for (const engine::InstrumentTableEntry& entry : project.instrument_table) {
            json params = json::object();
            for (const auto& [key, value] : entry.plugin_preset_params) {
                params[key] = value;
            }
            list.push_back(json{{"type", instrument_type_name(entry.type)},
                                {"sampleId", entry.sample_id},
                                {"pluginId", entry.plugin_id},
                                {"pluginPresetParams", std::move(params)},
                                {"workspaceId", entry.workspace_id},
                                {"boundTracks", entry.bound_tracks}});
        }
        return query_success(std::move(list));
    }
    if (op == "getTransport") {
        const audio::EngineSnapshot& snap = audio.snapshot();
        return query_success(json{{"playing", snap.transport_playing},
                                  {"orderPos", snap.order_pos},
                                  {"patternIndex", snap.pattern_index},
                                  {"row", snap.row},
                                  {"tick", snap.tick},
                                  {"bpm", snap.bpm},
                                  {"speed", snap.speed}});
    }
    if (op == "getWorkspace") {
        json nodes = json::array();
        for (const graph::Node& node : session.workspace().nodes()) {
            nodes.push_back(json{{"workspaceId", node.workspace_id},
                                 {"pluginId", node.plugin_id},
                                 {"displayName", node.display_name},
                                 {"kind", node_kind_name(node.kind)},
                                 {"inputs", ports_to_json(node.inputs)},
                                 {"outputs", ports_to_json(node.outputs)},
                                 {"volume", node.volume},
                                 {"pan", node.pan},
                                 {"bypass", node.bypass}});
        }
        json cables = json::array();
        for (const graph::Cable& cable : session.workspace().cables()) {
            cables.push_back(
                json{{"id", cable.id},
                     {"source",
                      json{{"nodeId", cable.source.node_id}, {"portId", cable.source.port_id}}},
                     {"dest", json{{"nodeId", cable.dest.node_id}, {"portId", cable.dest.port_id}}},
                     {"mode", cable.mode == graph::CableMode::kTap ? "tap" : "reroute"},
                     {"srcKind", graph::port_kind_name(cable.src_kind)},
                     {"dstKind", graph::port_kind_name(cable.dst_kind)}});
        }
        return query_success(json{{"nodes", std::move(nodes)}, {"cables", std::move(cables)}});
    }
    if (op == "getPluginParams") {
        std::string workspace_id;
        if (!get_string(query, "workspaceId", workspace_id)) {
            return failure(make_error(-1, kErrInvalidField, "workspaceId must be a string"));
        }
        if (plugins::NtpInstance* ntp = session.plugin_instance(workspace_id)) {
            json params = json::array();
            for (const ntp::ParamDef& param : ntp->manifest().params) {
                params.push_back(json{{"key", param.key},
                                      {"label", param.label},
                                      {"min", param.min},
                                      {"max", param.max},
                                      {"def", param.def},
                                      {"value", ntp->param_value(param.key)}});
            }
            return query_success(json{
                {"workspaceId", workspace_id}, {"kind", "ntp"}, {"params", std::move(params)}});
        }
        if (ext::ClapPlugin* clap = session.clap_instance(workspace_id)) {
            json params = json::array();
            for (const ext::ClapParamInfo& param : clap->params()) {
                params.push_back(json{{"key", param.id},
                                      {"label", param.name},
                                      {"module", param.module},
                                      {"min", param.min},
                                      {"max", param.max},
                                      {"def", param.def},
                                      {"value", clap->param_value(param.id)}});
            }
            return query_success(json{
                {"workspaceId", workspace_id}, {"kind", "clap"}, {"params", std::move(params)}});
        }
        if (ext::Vst3Plugin* vst3 = session.vst3_instance(workspace_id)) {
            json params = json::array();
            for (const ext::Vst3ParamInfo& param : vst3->params()) {
                params.push_back(json{{"key", param.id},
                                      {"label", param.title},
                                      {"min", 0.0},
                                      {"max", 1.0},
                                      {"def", param.def_normalized},
                                      {"automatable", param.automatable},
                                      {"value", vst3->param_value(param.id)}});
            }
            return query_success(json{
                {"workspaceId", workspace_id}, {"kind", "vst3"}, {"params", std::move(params)}});
        }
        return failure(
            make_error(-1, kErrNotFound,
                       "workspace node '" + workspace_id + "' is not a live plugin instance"));
    }
    if (op == "getSchema") {
        static const json kSchema = build_schema();
        return query_success(kSchema);
    }
    return failure(make_error(-1, kErrInvalidOp, "unknown query op: " + op));
}

// The machine-readable schema document (web getSchema shape). Field
// specs are terse name:type strings; summaries stay one line.
json build_schema() {
    auto op_doc = [](const char* op, const char* summary, std::vector<std::string> fields) {
        return json{{"op", op}, {"summary", summary}, {"fields", std::move(fields)}};
    };
    json commands = json::array({
        op_doc("setCell", "Merge a partial cell onto (pattern, row, channel).",
               {"patternId:int", "row:int", "channel:int",
                "cell:{note?,instrument?,volume?,effect?,effectParam?,boundIndex?}"}),
        op_doc("clearCell", "Reset the cell to empty.",
               {"patternId:int", "row:int", "channel:int"}),
        op_doc("setNoteOff", "Place a note-off (voice release) cell.",
               {"patternId:int", "row:int", "channel:int"}),
        op_doc("setRange", "Merge a 2D cell grid onto a rectangle of the pattern.",
               {"patternId:int", "rowStart:int", "channels:int[]", "cells:cell[][]"}),
        op_doc("setBpm", "Set song BPM (32..999).", {"value:number"}),
        op_doc("setSpeed", "Set ticks-per-row (1..31).", {"value:int"}),
        op_doc("renameSample", "Rename a loaded sample.", {"sampleId:int", "name:string"}),
        op_doc("setSampleMeta",
               "Patch sample metadata (name/baseNote/finetune/volume/pan/loops/stretchRatio/"
               "category). Audio data is read-only.",
               {"sampleId:int", "patch:object"}),
        op_doc("setSampleStretchRatio", "Set stretchRatio directly (0.01..100).",
               {"sampleId:int", "ratio:number"}),
        op_doc("conformSampleToRows",
               "Set stretchRatio so the sample spans `rows` rows at current bpm/speed.",
               {"sampleId:int", "rows:int", "patternId?:int"}),
        op_doc("setInstrumentSlot", "Set an instrument-table slot (1-based).",
               {"slot:int", "entry:{type,sampleId,pluginId,pluginPresetParams,workspaceId,"
                            "boundTracks?}"}),
        op_doc("clearInstrumentSlot", "Reset a slot to the default sample entry.", {"slot:int"}),
        op_doc("addSeqNote", "Insert a note into a sequence layer.",
               {"patternId:int", "channel:int", "layerIndex:int",
                "note:{pitch,startTick,durationTicks,velocity}"}),
        op_doc("removeSeqNote", "Remove the note at noteIndex.",
               {"patternId:int", "channel:int", "layerIndex:int", "noteIndex:int"}),
        op_doc("updateSeqNote", "Replace the note at noteIndex.",
               {"patternId:int", "channel:int", "layerIndex:int", "noteIndex:int", "note:object"}),
        op_doc("setSeqLayerInstrument", "Assign an instrument-table slot to a layer (0 = silent).",
               {"patternId:int", "channel:int", "layerIndex:int", "instrument:int"}),
        op_doc("setSeqLayerEnabled", "Mute/unmute a sequence layer.",
               {"patternId:int", "channel:int", "layerIndex:int", "enabled:bool"}),
        op_doc("play", "Start the transport.", {}),
        op_doc("stop", "Stop the transport.", {}),
        op_doc("newProject", "Replace the open project with a fresh default one.", {}),
        op_doc("loadProject", "Load a project file (.ftrk, .mod, …).", {"path:string"}),
        op_doc("saveProject", "Save the project as .ftrk.", {"path:string"}),
        op_doc("exportProject", "Offline render (blocks until encoded).",
               {"path:string", "format?:wav|ogg|mp3", "wavDepth?:pcm16|pcm24|float32",
                "oggQuality?:number", "mp3BitrateKbps?:int", "mp3Quality?:int", "sampleRate?:int",
                "tailSeconds?:number", "startOrder?:int", "endOrder?:int", "stemMask?:int",
                "stemZip?:bool", "metadata?:object", "post?:object"}),
        op_doc("loadSampleFile", "Load an audio file into a sample slot (1..31).",
               {"slot:int", "path:string"}),
        op_doc("loadSampleData", "Upload sample audio (base64 wav/ogg/mp3) into a slot.",
               {"slot:int", "dataBase64:string", "name?:string"}),
        op_doc("clearSample", "Clear a sample slot.", {"slot:int"}),
        op_doc("addWorkspaceNode", "Create a utility workspace node.",
               {"kind:sum|extMidiIn|extMidiOut"}),
        op_doc("addPluginNode", "Instantiate a catalogued NTP plugin as a workspace node.",
               {"pluginId:string"}),
        op_doc("removeWorkspaceNode", "Remove a user-created workspace node.",
               {"workspaceId:string"}),
        op_doc("addCable", "Patch a cable between two ports.",
               {"source:{nodeId,portId}", "dest:{nodeId,portId}", "mode?:tap|reroute"}),
        op_doc("removeCable", "Remove a cable.", {"cableId:string"}),
        op_doc("setCableMode", "Toggle a cable between tap and reroute.",
               {"cableId:string", "mode:tap|reroute"}),
        op_doc("setPluginParam", "Set a plugin parameter on a live workspace instance.",
               {"workspaceId:string", "key:string(NTP)|int(CLAP/VST3)", "value:number"}),
    });
    json queries = json::array({
        op_doc("getProjectSummary", "Scalar project overview.", {}),
        op_doc("getPatternList", "Per-pattern id/name/rows/filledCells/orderListPositions.", {}),
        op_doc("getPattern", "Full pattern including all cells.", {"patternId:int"}),
        op_doc("getRange", "Slice of cells, optionally filtered to channels.",
               {"patternId:int", "rowStart:int", "rowEnd:int(exclusive)", "channels?:int[]"}),
        op_doc("getOrderList", "The order list (pattern ids).", {}),
        op_doc("getSamples", "All sample metadata (audio data stripped).", {}),
        op_doc("getInstrumentTable", "The instrument table.", {}),
        op_doc("getSeqLayer", "A sequence layer's notes.",
               {"patternId:int", "channel:int", "layerIndex:int"}),
        op_doc("getSeqLayerList", "Every existing sequence layer in a pattern.", {"patternId:int"}),
        op_doc("getTransport", "Playback state and position.", {}),
        op_doc("getWorkspace", "Workspace discovery: nodes with ports, and cables.", {}),
        op_doc("getPluginParams", "Parameter list + values of a live plugin node.",
               {"workspaceId:string"}),
        op_doc("getSchema", "This document.", {}),
    });
    json notes = json::array();
    notes.push_back("Auth: send {type:'hello', token} as the first frame; every later frame is "
                    "{type:'request', requestId, kind:'execute'|'read', commands|query, opts}.");
    notes.push_back("Every batch validates against pre-batch state; intra-batch references are "
                    "unsupported (web parity).");
    notes.push_back(
        "Pattern/order-list structure ops (insertRow, createPattern, setOrderList, "
        "setChannels, …) return code 'unsupported' until the session grows that surface.");
    notes.push_back("Sequence layers are preallocated: layerIndex 0..3 is always addressable; "
                    "addSeqLayer/removeSeqLayer return 'unsupported'.");
    notes.push_back("patternId is the persistent pattern id, not an order-list index.");
    return json{{"version", kLocalApiVersion},
                {"limits", json{{"maxCommandsPerBatch", kMaxCommandsPerBatch},
                                {"maxPayloadBytes", kMaxMessageBytes},
                                {"maxQueuedRequests", kMaxQueuedRequests}}},
                {"commands", std::move(commands)},
                {"queries", std::move(queries)},
                {"notes", std::move(notes)}};
}

std::string describe_batch(const json& commands, const std::string& undo_description) {
    if (!undo_description.empty()) {
        return undo_description;
    }
    if (!commands.is_array() || commands.empty()) {
        return "(empty batch)";
    }
    std::string op = "(?)";
    if (commands[0].is_object()) {
        get_string(commands[0], "op", op);
    }
    if (commands.size() > 1) {
        op += " ×" + std::to_string(commands.size());
    }
    return op;
}

void send_json(const std::shared_ptr<ix::WebSocket>& socket, const json& frame) {
    if (socket != nullptr) {
        socket->send(frame.dump());
    }
}

json protocol_error(const char* code, const std::string& message) {
    return json{{"type", "error"}, {"code", code}, {"message", message}};
}

} // namespace

// ── Token generation ─────────────────────────────────────────────────

std::string generate_token() {
    std::random_device device; // OS CSPRNG on every supported platform
    constexpr const char* kHex = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (int word = 0; word < 4; ++word) {
        std::uint32_t bits = device();
        for (int nibble = 0; nibble < 8; ++nibble) {
            token.push_back(kHex[bits & 0xFU]);
            bits >>= 4U;
        }
    }
    return token;
}

// ── Server lifecycle ─────────────────────────────────────────────────

LocalApiServer::LocalApiServer() = default;

LocalApiServer::~LocalApiServer() {
    stop();
}

bool LocalApiServer::start(int port, const std::string& token) {
    if (running_) {
        stop();
    }
    if (token.empty()) {
        error_ = "empty token";
        return false;
    }
    ix::initNetSystem(); // WSAStartup on Windows; no-op elsewhere

    auto server = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
    server->disablePerMessageDeflate(); // zlib is compiled out

    // Connection callback (server accept thread): register the message
    // callback per socket. The weak_ptr is what queued requests hold —
    // a departing client cannot dangle, and a locked send keeps the
    // socket alive for its duration.
    server->setOnConnectionCallback([this](const std::weak_ptr<ix::WebSocket>& socket,
                                           const std::shared_ptr<ix::ConnectionState>& state) {
        const std::shared_ptr<ix::WebSocket> shared = socket.lock();
        if (shared == nullptr) {
            return;
        }
        shared->setOnMessageCallback([this, socket, state](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                const std::lock_guard<std::mutex> lock(mutex_);
                ClientInfo client;
                client.id = state->getId();
                client.address =
                    state->getRemoteIp() + ":" + std::to_string(state->getRemotePort());
                client.connected_at = std::chrono::system_clock::now();
                clients_.push_back(std::move(client));
                break;
            }
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error: {
                const std::lock_guard<std::mutex> lock(mutex_);
                clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                              [&state](const ClientInfo& c) {
                                                  return c.id == state->getId();
                                              }),
                               clients_.end());
                break;
            }
            case ix::WebSocketMessageType::Message:
                on_client_message(socket.lock(), state->getId(), msg->str);
                break;
            default:
                break; // ping/pong/fragment — the library handles these
            }
        });
    });

    const auto listen_result = server->listen();
    if (!listen_result.first) {
        error_ = listen_result.second;
        return false;
    }
    server->start();

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        token_ = token;
        queue_.clear();
        queued_bytes_ = 0;
        clients_.clear();
    }
    server_ = std::move(server);
    port_ = port;
    running_ = true;
    error_.clear();
    return true;
}

void LocalApiServer::stop() {
    if (server_ != nullptr) {
        server_->stop(); // closes clients, joins connection threads
        server_.reset();
    }
    running_ = false;
    port_ = 0;
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    queued_bytes_ = 0;
    clients_.clear();
}

// Server-thread frame triage: auth handshake, ping, shape/size checks,
// enqueue. Everything else — anything touching the session — waits for
// process_pending on the UI thread.
void LocalApiServer::on_client_message(const std::shared_ptr<ix::WebSocket>& socket,
                                       const std::string& connection_id,
                                       const std::string& payload) {
    if (socket == nullptr) {
        return;
    }
    if (payload.size() > kMaxMessageBytes) {
        send_json(socket,
                  protocol_error(kErrPayloadTooLarge,
                                 "message exceeds " + std::to_string(kMaxMessageBytes) + " bytes"));
        push_log("denied", "oversize message (" + std::to_string(payload.size()) + " bytes)",
                 false);
        return;
    }
    const json frame = json::parse(payload, nullptr, /*allow_exceptions=*/false);
    if (frame.is_discarded() || !frame.is_object()) {
        send_json(socket, protocol_error(kErrInvalidField, "malformed JSON frame"));
        push_log("error", "malformed JSON frame", false);
        return;
    }
    std::string type;
    get_string(frame, "type", type);

    if (type == "ping") {
        send_json(socket, json{{"type", "pong"}});
        return;
    }

    if (type == "hello") {
        std::string token;
        get_string(frame, "token", token);
        bool authed = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            authed = !token_.empty() && token == token_;
            if (authed) {
                for (ClientInfo& client : clients_) {
                    if (client.id == connection_id) {
                        client.authed = true;
                        break;
                    }
                }
            }
        }
        if (!authed) {
            send_json(socket, protocol_error(kErrUnauthorized, "bad token"));
            push_log("denied", "hello with bad token", false);
            socket->close();
            return;
        }
        send_json(socket, json{{"type", "welcome"},
                               {"role", "active"},
                               {"version", kLocalApiVersion},
                               {"app", kAppName},
                               {"appVersion", kVersionString}});
        push_log("read", "hello (client authenticated)", true);
        return;
    }

    if (type == "request") {
        bool authed = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const ClientInfo& client : clients_) {
                if (client.id == connection_id) {
                    authed = client.authed;
                    break;
                }
            }
        }
        if (!authed) {
            send_json(socket, protocol_error(kErrUnauthorized, "hello handshake required"));
            push_log("denied", "request before hello", false);
            socket->close();
            return;
        }
        std::string request_id;
        std::string kind;
        if (!get_string(frame, "requestId", request_id) || !get_string(frame, "kind", kind) ||
            (kind != "execute" && kind != "read")) {
            send_json(socket, protocol_error(kErrInvalidField,
                                             "request needs requestId and kind execute|read"));
            push_log("error", "malformed request frame", false);
            return;
        }
        bool accepted = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() < kMaxQueuedRequests &&
                queued_bytes_ + payload.size() <= kMaxQueuedBytes) {
                queue_.push_back(PendingRequest{socket, connection_id, payload, payload.size()});
                queued_bytes_ += payload.size();
                accepted = true;
            }
        }
        if (!accepted) {
            send_json(socket, protocol_error(kErrRateLimited, "request queue full — slow down"));
            push_log("denied", "request queue full", false);
        }
        return;
    }

    send_json(socket, protocol_error(kErrInvalidField, "unknown frame type '" + type + "'"));
    push_log("error", "unknown frame type", false);
}

void LocalApiServer::process_pending(app::ProjectSession& session, audio::AudioEngine& audio) {
    if (!running_) {
        return;
    }
    std::deque<PendingRequest> batch;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(queue_);
        queued_bytes_ = 0;
    }

    for (const PendingRequest& pending : batch) {
        // Parsed once for triage on the server thread, parsed again
        // here — the request text is the honest queue currency (no
        // json object crosses the header).
        const json frame = json::parse(pending.frame, nullptr, /*allow_exceptions=*/false);
        if (frame.is_discarded()) {
            continue; // triage already validated; cannot happen
        }
        std::string request_id;
        std::string kind;
        get_string(frame, "requestId", request_id);
        get_string(frame, "kind", kind);

        static const json kEmptyArray = json::array();
        static const json kEmptyObject = json::object();
        json result;
        std::string description;
        if (kind == "execute") {
            const auto commands_it = frame.find("commands");
            const json& commands = commands_it != frame.end() ? *commands_it : kEmptyArray;
            const auto opts_it = frame.find("opts");
            const json& opts = opts_it != frame.end() ? *opts_it : kEmptyObject;
            result = execute_batch(session, commands, opts);
            std::string undo_description;
            if (opts.is_object()) {
                get_string(opts, "undoDescription", undo_description);
            }
            description = describe_batch(commands, undo_description);
        } else {
            const auto query_it = frame.find("query");
            const json& query = query_it != frame.end() ? *query_it : kEmptyObject;
            result = run_query(session, audio, query);
            description = "(?)";
            if (query.is_object()) {
                get_string(query, "op", description);
            }
        }
        bool ok = false;
        get_bool(result, "ok", ok);

        send_json(
            pending.socket.lock(),
            json{{"type", "reply"}, {"requestId", request_id}, {"result", std::move(result)}});
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (ClientInfo& client : clients_) {
                if (client.id == pending.connection_id) {
                    ++client.requests;
                    break;
                }
            }
        }
        push_log(kind, description, ok);
    }
}

std::vector<ClientInfo> LocalApiServer::clients() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

std::vector<LogEntry> LocalApiServer::log_tail() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return {log_.begin(), log_.end()};
}

void LocalApiServer::clear_log() {
    const std::lock_guard<std::mutex> lock(mutex_);
    log_.clear();
}

void LocalApiServer::push_log(const std::string& kind, const std::string& description, bool ok) {
    constexpr std::size_t kMaxLogEntries = 128;
    const std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back(LogEntry{std::chrono::system_clock::now(), kind, description, ok});
    while (log_.size() > kMaxLogEntries) {
        log_.pop_front();
    }
}

} // namespace nt::api
