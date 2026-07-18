#include "io/settings.h"

#include "platform/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace nt::io {

namespace {

using nlohmann::json;

// Reads a key into `out` only if present with a compatible type;
// absent or mistyped keys keep the default (tolerant-load contract).
template <typename T>
void read_if(const json& j, const char* key, T& out) {
    if (const auto it = j.find(key); it != j.end()) {
        try {
            out = it->get<T>();
        } catch (const json::exception&) { // NOLINT(bugprone-empty-catch)
            // Tolerant-load contract: a mistyped value keeps the
            // default rather than failing startup.
        }
    }
}

} // namespace

Settings load_settings(const std::filesystem::path& path) {
    Settings settings;

    std::ifstream file(path);
    if (!file) {
        return settings;
    }

    const json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        std::fprintf(stderr, "settings: malformed %s, using defaults\n", path.c_str());
        return settings;
    }

    read_if(j, "schema", settings.schema);
    read_if(j, "theme_id", settings.theme_id);
    read_if(j, "crt_enabled", settings.crt_enabled);
    read_if(j, "crt_intensity", settings.crt_intensity);
    read_if(j, "window_width", settings.window_width);
    read_if(j, "window_height", settings.window_height);
    read_if(j, "cable_resolution", settings.cable_resolution);
    read_if(j, "cable_gravity", settings.cable_gravity);
    read_if(j, "cable_damping", settings.cable_damping);
    read_if(j, "cable_slack", settings.cable_slack);
    read_if(j, "cable_iterations", settings.cable_iterations);
    read_if(j, "cable_thickness", settings.cable_thickness);

    // Defensive clamps mirroring the web loader (cableSettings.ts:78-83)
    // so corrupted storage can never explode the rope simulator.
    settings.cable_resolution = std::clamp(settings.cable_resolution, 1, 64);
    settings.cable_gravity = std::max(0.0F, settings.cable_gravity);
    settings.cable_damping = std::clamp(settings.cable_damping, 0.0F, 0.95F);
    settings.cable_slack = std::clamp(settings.cable_slack, 0.5F, 3.0F);
    settings.cable_iterations = std::clamp(settings.cable_iterations, 1, 16);
    settings.cable_thickness = std::clamp(settings.cable_thickness, 0.5F, 16.0F);
    return settings;
}

bool save_settings(const Settings& settings, const std::filesystem::path& path) {
    const json j = {
        {"schema", settings.schema},
        {"theme_id", settings.theme_id},
        {"crt_enabled", settings.crt_enabled},
        {"crt_intensity", settings.crt_intensity},
        {"window_width", settings.window_width},
        {"window_height", settings.window_height},
        {"cable_resolution", settings.cable_resolution},
        {"cable_gravity", settings.cable_gravity},
        {"cable_damping", settings.cable_damping},
        {"cable_slack", settings.cable_slack},
        {"cable_iterations", settings.cable_iterations},
        {"cable_thickness", settings.cable_thickness},
    };

    std::ofstream file(path);
    if (!file) {
        return false;
    }
    file << j.dump(2) << '\n';
    return file.good();
}

std::filesystem::path default_settings_path() {
    return platform::config_dir() / "settings.json";
}

} // namespace nt::io
