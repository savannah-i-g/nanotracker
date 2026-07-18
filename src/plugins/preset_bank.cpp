#include "plugins/preset_bank.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <ctime>
#include <fstream>

namespace nt::plugins {

namespace {

using nlohmann::json;

UserPreset preset_from_json(const json& j) {
    UserPreset preset;
    preset.preset_id = j.value("presetId", "");
    preset.name = j.value("name", "");
    preset.created_at = j.value("createdAt", std::int64_t{0});
    preset.updated_at = j.value("updatedAt", std::int64_t{0});
    const json values = j.value("values", json::object());
    for (const auto& [key, value] : values.items()) {
        if (value.is_number()) {
            preset.values.emplace_back(key, value.get<double>());
        }
    }
    return preset;
}

json preset_to_json(const UserPreset& preset) {
    json values = json::object();
    for (const auto& [key, value] : preset.values) {
        values[key] = value;
    }
    return json{{"presetId", preset.preset_id},
                {"name", preset.name},
                {"createdAt", preset.created_at},
                {"updatedAt", preset.updated_at},
                {"values", std::move(values)}};
}

} // namespace

PresetBank::PresetBank(std::filesystem::path library_dir) : library_dir_(std::move(library_dir)) {}

std::filesystem::path PresetBank::library_file(const std::string& plugin_id) const {
    // Sanitised id: anything outside [a-z0-9.-] becomes '_' so plugin
    // ids can never escape the presets directory.
    std::string name;
    for (const char c : plugin_id) {
        const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        name +=
            (std::isalnum(static_cast<unsigned char>(lower)) != 0 || lower == '.' || lower == '-')
                ? lower
                : '_';
    }
    return library_dir_ / (name + ".json");
}

std::vector<UserPreset> PresetBank::library(const std::string& plugin_id) const {
    std::vector<UserPreset> out;
    std::ifstream file(library_file(plugin_id));
    if (!file) {
        return out;
    }
    const json root = json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        return out;
    }
    for (const json& entry : root) {
        if (entry.is_object()) {
            out.push_back(preset_from_json(entry));
        }
    }
    return out;
}

bool PresetBank::save_to_library(const std::string& plugin_id, const std::string& name,
                                 const std::vector<std::pair<std::string, double>>& values) {
    std::vector<UserPreset> existing = library(plugin_id);
    UserPreset preset;
    preset.preset_id = "user-" + std::to_string(next_id_++) + "-" +
                       std::to_string(static_cast<std::int64_t>(std::time(nullptr)));
    preset.name = name;
    preset.values = values;
    preset.created_at = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    preset.updated_at = preset.created_at;
    existing.push_back(std::move(preset));

    std::error_code ec;
    std::filesystem::create_directories(library_dir_, ec);
    json root = json::array();
    for (const UserPreset& p : existing) {
        root.push_back(preset_to_json(p));
    }
    std::ofstream file(library_file(plugin_id));
    if (!file) {
        return false;
    }
    file << root.dump(2) << '\n';
    return file.good();
}

void PresetBank::adopt_pprs(const std::string& pprs_json, std::vector<std::string>& warnings) {
    project_.clear();
    if (pprs_json.empty()) {
        return;
    }
    const json root = json::parse(pprs_json, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        warnings.emplace_back("presets: PPRS payload is not a valid entry array — ignored");
        return;
    }
    for (const json& entry : root) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string instance_id = entry.value("instanceId", "");
        if (instance_id.empty()) {
            continue;
        }
        std::vector<UserPreset>& records = project_[instance_id];
        for (const json& record : entry.value("projectPresets", json::array())) {
            if (record.is_object()) {
                records.push_back(preset_from_json(record));
            }
        }
    }
}

void PresetBank::clear_project() {
    project_.clear();
}

const std::vector<UserPreset>* PresetBank::project_presets(const std::string& instance_id) const {
    const auto it = project_.find(instance_id);
    return it != project_.end() ? &it->second : nullptr;
}

void PresetBank::save_to_project(const std::string& instance_id, const std::string& name,
                                 const std::vector<std::pair<std::string, double>>& values) {
    UserPreset preset;
    preset.preset_id = "proj-" + std::to_string(next_id_++);
    preset.name = name;
    preset.values = values;
    preset.created_at = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    preset.updated_at = preset.created_at;
    project_[instance_id].push_back(std::move(preset));
}

std::string PresetBank::to_pprs_json() const {
    json root = json::array();
    for (const auto& [instance_id, records] : project_) {
        json entry = json::object();
        entry["instanceId"] = instance_id;
        json presets = json::array();
        for (const UserPreset& preset : records) {
            presets.push_back(preset_to_json(preset));
        }
        entry["projectPresets"] = std::move(presets);
        root.push_back(std::move(entry));
    }
    return root.dump();
}

} // namespace nt::plugins
