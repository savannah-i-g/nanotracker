// plugins/preset_bank — user preset storage. Two scopes, mirroring the
// web host (pluginTypes.ts UserPresetRecord):
//   library — per-plugin JSON files in the user config directory
//             (the web kept these in IndexedDB);
//   project — PPRS-block records per workspace instance, carried in
//             the .ftrk so songs travel with their sounds.
// Factory presets live in the manifest and never pass through here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nt::plugins {

struct UserPreset {
    std::string preset_id;
    std::string name;
    std::vector<std::pair<std::string, double>> values;
    std::int64_t created_at = 0; // ms epoch (web parity); 0 = unknown
    std::int64_t updated_at = 0;
};

class PresetBank {
public:
    // `library_dir` — e.g. config_dir()/presets. Created on first save.
    explicit PresetBank(std::filesystem::path library_dir);

    // ── Library scope ────────────────────────────────────────────────
    [[nodiscard]] std::vector<UserPreset> library(const std::string& plugin_id) const;
    bool save_to_library(const std::string& plugin_id, const std::string& name,
                         const std::vector<std::pair<std::string, double>>& values);

    // ── Project scope (PPRS) ─────────────────────────────────────────
    // Replaces project records with the parsed payload; malformed
    // entries are skipped with a warning.
    void adopt_pprs(const std::string& pprs_json, std::vector<std::string>& warnings);
    void clear_project();
    [[nodiscard]] const std::vector<UserPreset>*
    project_presets(const std::string& instance_id) const;
    void save_to_project(const std::string& instance_id, const std::string& name,
                         const std::vector<std::pair<std::string, double>>& values);
    // Serialises project records back to the PPRS shape (writer stage).
    [[nodiscard]] std::string to_pprs_json() const;

private:
    [[nodiscard]] std::filesystem::path library_file(const std::string& plugin_id) const;

    std::filesystem::path library_dir_;
    std::map<std::string, std::vector<UserPreset>> project_; // instanceId → records
    int next_id_ = 1;
};

} // namespace nt::plugins
