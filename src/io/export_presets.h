// io/export_presets — named ExportOptions bundles for the export
// window. Built-ins carry the web's exportPresets.ts values across
// where the native pipeline can express them; user presets persist as
// JSON in the config directory (the web kept them in localStorage).
//
// Honest adaptations from the web preset shape:
//   formats[]  — the native renderer encodes one format per run, so a
//                preset holds a single ExportFormat (the web preset's
//                primary format). "Everything (WAV/MP3/OGG zip)" has
//                no native equivalent and is not carried.
//   renderMode — offline only; the realtime-capture mode existed to
//                work around ScriptProcessorNode (fixed natively,
//                FIXES.md #7), so "Live Capture (debug)" is dropped.
//   dither / cuePointsAtPatterns / offlineBypassCompressor — no
//                counterpart in ExportOptions; dropped.
//   oggQuality — web 0..10 maps to vorbis VBR 0.0..1.0.
#pragma once

#include "io/export_render.h"

#include <filesystem>
#include <string>
#include <vector>

namespace nt::io {

struct ExportPreset {
    std::string id; // stable key; "user-…" for saved presets
    std::string name;
    ExportOptions options;
};

// The adapted web built-ins, in display order. Index 0 is the default
// options bundle a fresh export window starts from.
[[nodiscard]] const std::vector<ExportPreset>& builtin_export_presets();

[[nodiscard]] bool is_builtin_export_preset(const std::string& id);

// User preset storage: one JSON array at `file` (conventionally
// config_dir()/export_presets.json), read on construction. Built-ins
// are resolvable through find() but never written; save/delete/rename
// touch user presets only and persist immediately. Serialisation
// covers the whole ExportOptions including post and metadata, with the
// settings.cpp tolerant-load contract: absent or mistyped keys keep
// defaults, a malformed file yields an empty list.
class ExportPresetStore {
public:
    explicit ExportPresetStore(std::filesystem::path file);

    // Re-reads the backing file, replacing in-memory user presets.
    void reload();

    [[nodiscard]] const std::vector<ExportPreset>& user_presets() const { return user_; }

    // Looks up built-ins first, then user presets; null when unknown.
    [[nodiscard]] const ExportPreset* find(const std::string& id) const;

    // Appends a new user preset (duplicate names allowed, as on the
    // web) and persists. Returns the new id, empty on write failure.
    std::string save_user_preset(const std::string& name, const ExportOptions& options);

    // Both refuse built-in and unknown ids; false also on write failure.
    bool delete_user_preset(const std::string& id);
    bool rename_user_preset(const std::string& id, const std::string& name);

private:
    [[nodiscard]] bool persist() const;

    std::filesystem::path file_;
    std::vector<ExportPreset> user_;
    int next_id_ = 1;
};

} // namespace nt::io
