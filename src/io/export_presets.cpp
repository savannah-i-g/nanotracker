#include "io/export_presets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <utility>

namespace nt::io {

namespace {

using nlohmann::json;

// Reads a key into `out` only if present with a compatible type
// (settings.cpp tolerant-load contract).
template <typename T>
void read_if(const json& j, const char* key, T& out) {
    if (const auto it = j.find(key); it != j.end()) {
        try {
            out = it->get<T>();
        } catch (const json::exception&) { // NOLINT(bugprone-empty-catch)
            // A mistyped value keeps the default rather than failing
            // the whole preset list.
        }
    }
}

// Enums travel as strings so the file stays hand-readable and enum
// reordering can never silently change stored presets. Unknown strings
// keep the caller's default (same tolerance as read_if).

const char* format_to_string(ExportFormat format) {
    switch (format) {
    case ExportFormat::kOgg:
        return "ogg";
    case ExportFormat::kMp3:
        return "mp3";
    case ExportFormat::kWav:
        break;
    }
    return "wav";
}

void format_from_string(const std::string& text, ExportFormat& out) {
    if (text == "wav") {
        out = ExportFormat::kWav;
    } else if (text == "ogg") {
        out = ExportFormat::kOgg;
    } else if (text == "mp3") {
        out = ExportFormat::kMp3;
    }
}

const char* depth_to_string(WavDepth depth) {
    switch (depth) {
    case WavDepth::kPcm24:
        return "pcm24";
    case WavDepth::kFloat32:
        return "float32";
    case WavDepth::kPcm16:
        break;
    }
    return "pcm16";
}

void depth_from_string(const std::string& text, WavDepth& out) {
    if (text == "pcm16") {
        out = WavDepth::kPcm16;
    } else if (text == "pcm24") {
        out = WavDepth::kPcm24;
    } else if (text == "float32") {
        out = WavDepth::kFloat32;
    }
}

const char* shape_to_string(FadeShape shape) {
    return shape == FadeShape::kEqualPower ? "equal_power" : "linear";
}

void shape_from_string(const std::string& text, FadeShape& out) {
    if (text == "linear") {
        out = FadeShape::kLinear;
    } else if (text == "equal_power") {
        out = FadeShape::kEqualPower;
    }
}

const char* normalize_to_string(NormalizeMode mode) {
    switch (mode) {
    case NormalizeMode::kPeak:
        return "peak";
    case NormalizeMode::kTruePeak:
        return "true_peak";
    case NormalizeMode::kLufs:
        return "lufs";
    case NormalizeMode::kNone:
        break;
    }
    return "none";
}

void normalize_from_string(const std::string& text, NormalizeMode& out) {
    if (text == "none") {
        out = NormalizeMode::kNone;
    } else if (text == "peak") {
        out = NormalizeMode::kPeak;
    } else if (text == "true_peak") {
        out = NormalizeMode::kTruePeak;
    } else if (text == "lufs") {
        out = NormalizeMode::kLufs;
    }
}

template <typename E, void (*Parse)(const std::string&, E&)>
void read_enum(const json& j, const char* key, E& out) {
    std::string text;
    read_if(j, key, text);
    Parse(text, out);
}

json options_to_json(const ExportOptions& options) {
    return json{
        {"format", format_to_string(options.format)},
        {"wav_depth", depth_to_string(options.wav_depth)},
        {"ogg_quality", options.ogg_quality},
        {"mp3_bitrate_kbps", options.mp3_bitrate_kbps},
        {"mp3_quality", options.mp3_quality},
        {"sample_rate", options.sample_rate},
        {"tail_seconds", options.tail_seconds},
        {"start_order", options.start_order},
        {"end_order", options.end_order},
        {"stem_mask", options.stem_mask},
        {"stem_zip", options.stem_zip},
        {"post",
         json{
             {"fade_in_seconds", options.post.fade_in_seconds},
             {"fade_out_seconds", options.post.fade_out_seconds},
             {"fade_in_shape", shape_to_string(options.post.fade_in_shape)},
             {"fade_out_shape", shape_to_string(options.post.fade_out_shape)},
             {"normalize", normalize_to_string(options.post.normalize)},
             {"normalize_target_db", options.post.normalize_target_db},
         }},
        {"metadata",
         json{
             {"title", options.metadata.title},
             {"artist", options.metadata.artist},
             {"album", options.metadata.album},
             {"date", options.metadata.date},
             {"comment", options.metadata.comment},
         }},
    };
}

ExportOptions options_from_json(const json& j) {
    ExportOptions options;
    read_enum<ExportFormat, format_from_string>(j, "format", options.format);
    read_enum<WavDepth, depth_from_string>(j, "wav_depth", options.wav_depth);
    read_if(j, "ogg_quality", options.ogg_quality);
    read_if(j, "mp3_bitrate_kbps", options.mp3_bitrate_kbps);
    read_if(j, "mp3_quality", options.mp3_quality);
    read_if(j, "sample_rate", options.sample_rate);
    read_if(j, "tail_seconds", options.tail_seconds);
    read_if(j, "start_order", options.start_order);
    read_if(j, "end_order", options.end_order);
    read_if(j, "stem_mask", options.stem_mask);
    read_if(j, "stem_zip", options.stem_zip);
    if (const auto post = j.find("post"); post != j.end() && post->is_object()) {
        read_if(*post, "fade_in_seconds", options.post.fade_in_seconds);
        read_if(*post, "fade_out_seconds", options.post.fade_out_seconds);
        read_enum<FadeShape, shape_from_string>(*post, "fade_in_shape", options.post.fade_in_shape);
        read_enum<FadeShape, shape_from_string>(*post, "fade_out_shape",
                                                options.post.fade_out_shape);
        read_enum<NormalizeMode, normalize_from_string>(*post, "normalize", options.post.normalize);
        read_if(*post, "normalize_target_db", options.post.normalize_target_db);
    }
    if (const auto meta = j.find("metadata"); meta != j.end() && meta->is_object()) {
        read_if(*meta, "title", options.metadata.title);
        read_if(*meta, "artist", options.metadata.artist);
        read_if(*meta, "album", options.metadata.album);
        read_if(*meta, "date", options.metadata.date);
        read_if(*meta, "comment", options.metadata.comment);
    }
    return options;
}

// Web BUILTIN_PRESETS (exportPresets.ts:41) mapped onto ExportOptions;
// the header lists what each adaptation drops.
std::vector<ExportPreset> make_builtins() {
    std::vector<ExportPreset> builtins;

    { // default-wav16: the plain quick bounce.
        ExportPreset preset{.id = "default-wav16", .name = "WAV 16-BIT", .options = {}};
        preset.options.format = ExportFormat::kWav;
        preset.options.wav_depth = WavDepth::kPcm16;
        preset.options.ogg_quality = 0.5F;
        preset.options.mp3_bitrate_kbps = 192;
        preset.options.tail_seconds = 1.0;
        preset.options.post.fade_out_seconds = 0.05;
        preset.options.post.normalize_target_db = -0.3;
        builtins.push_back(std::move(preset));
    }
    { // shareable-mp3: streaming-loudness master.
        ExportPreset preset{
            .id = "shareable-mp3", .name = "Shareable MP3 (192 kbps)", .options = {}};
        preset.options.format = ExportFormat::kMp3;
        preset.options.mp3_bitrate_kbps = 192;
        preset.options.ogg_quality = 0.5F;
        preset.options.tail_seconds = 2.0;
        preset.options.post.fade_out_seconds = 0.5;
        preset.options.post.normalize = NormalizeMode::kLufs;
        preset.options.post.normalize_target_db = -14.0;
        builtins.push_back(std::move(preset));
    }
    { // mastering-wav24: untouched signal, long tail.
        ExportPreset preset{.id = "mastering-wav24", .name = "Mastering WAV 24-bit", .options = {}};
        preset.options.format = ExportFormat::kWav;
        preset.options.wav_depth = WavDepth::kPcm24;
        preset.options.mp3_bitrate_kbps = 320;
        preset.options.ogg_quality = 0.8F;
        preset.options.tail_seconds = 3.0;
        preset.options.post.normalize_target_db = -0.3;
        builtins.push_back(std::move(preset));
    }
    { // stems-zip: every channel, bundled. An all-ones mask means "all
      // channels" — the renderer only walks bits below the project's
      // channel count, so the preset needs no project knowledge.
        ExportPreset preset{.id = "stems-zip", .name = "Stems for DAW (WAV 24)", .options = {}};
        preset.options.format = ExportFormat::kWav;
        preset.options.wav_depth = WavDepth::kPcm24;
        preset.options.ogg_quality = 0.5F;
        preset.options.mp3_bitrate_kbps = 192;
        preset.options.tail_seconds = 2.0;
        preset.options.stem_mask = ~0U;
        preset.options.stem_zip = true;
        preset.options.post.normalize_target_db = -0.3;
        builtins.push_back(std::move(preset));
    }
    return builtins;
}

} // namespace

const std::vector<ExportPreset>& builtin_export_presets() {
    static const std::vector<ExportPreset> kBuiltins = make_builtins();
    return kBuiltins;
}

bool is_builtin_export_preset(const std::string& id) {
    return std::ranges::any_of(builtin_export_presets(),
                               [&id](const ExportPreset& preset) { return preset.id == id; });
}

ExportPresetStore::ExportPresetStore(std::filesystem::path file) : file_(std::move(file)) {
    reload();
}

void ExportPresetStore::reload() {
    user_.clear();
    std::ifstream file(file_);
    if (!file) {
        return;
    }
    const json root = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_array()) {
        return;
    }
    for (const json& entry : root) {
        if (!entry.is_object()) {
            continue;
        }
        ExportPreset preset;
        read_if(entry, "id", preset.id);
        read_if(entry, "name", preset.name);
        if (preset.id.empty() || is_builtin_export_preset(preset.id)) {
            continue; // a built-in id in the file can never shadow the built-in
        }
        if (const auto options = entry.find("options");
            options != entry.end() && options->is_object()) {
            preset.options = options_from_json(*options);
        }
        user_.push_back(std::move(preset));
    }
}

const ExportPreset* ExportPresetStore::find(const std::string& id) const {
    for (const ExportPreset& preset : builtin_export_presets()) {
        if (preset.id == id) {
            return &preset;
        }
    }
    for (const ExportPreset& preset : user_) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

std::string ExportPresetStore::save_user_preset(const std::string& name,
                                                const ExportOptions& options) {
    ExportPreset preset;
    // Counter + wall clock, as in plugins/preset_bank: unique within a
    // run and across runs.
    preset.id = "user-" + std::to_string(next_id_++) + "-" +
                std::to_string(static_cast<std::int64_t>(std::time(nullptr)));
    preset.name = name;
    preset.options = options;
    user_.push_back(std::move(preset));
    if (!persist()) {
        user_.pop_back();
        return "";
    }
    return user_.back().id;
}

bool ExportPresetStore::delete_user_preset(const std::string& id) {
    for (auto it = user_.begin(); it != user_.end(); ++it) {
        if (it->id == id) {
            user_.erase(it);
            return persist();
        }
    }
    return false;
}

bool ExportPresetStore::rename_user_preset(const std::string& id, const std::string& name) {
    for (ExportPreset& preset : user_) {
        if (preset.id == id) {
            preset.name = name;
            return persist();
        }
    }
    return false;
}

bool ExportPresetStore::persist() const {
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    json root = json::array();
    for (const ExportPreset& preset : user_) {
        root.push_back(json{{"id", preset.id},
                            {"name", preset.name},
                            {"options", options_to_json(preset.options)}});
    }
    std::ofstream file(file_);
    if (!file) {
        return false;
    }
    file << root.dump(2) << '\n';
    return file.good();
}

} // namespace nt::io
