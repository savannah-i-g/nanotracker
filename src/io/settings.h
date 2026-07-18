// io/settings — application settings persisted as versioned JSON in the
// user config directory. One flat struct; subsystems read what they
// need. Unknown keys in the file are preserved-ignored (forward
// compatible); missing keys take defaults (backward compatible).
#pragma once

#include <filesystem>
#include <string>

namespace nt::io {

struct Settings {
    // Bumped only on incompatible layout changes; additions are free.
    int schema = 1;

    // Fresh-config default; files that saved a theme keep it —
    // save_settings always writes theme_id.
    std::string theme_id = "arctic-light";

    // User UI scale, multiplies font size and style metrics; clamped
    // 0.6–2.0.
    float ui_scale = 1.0F;

    bool crt_enabled = true;
    float crt_intensity = 0.35F; // 0 = off visually, 1 = full effect

    // Piano roll: preview added/drawn notes through the layer instrument.
    bool piano_roll_audition = true;

    int window_width = 1280;
    int window_height = 720;

    // Cable overlay physics/looks (web cableSettings.ts defaults).
    // resolution 1 = straight line, no physics; 16 = smooth rope.
    int cable_resolution = 16;
    float cable_gravity = 900.0F; // px/s^2
    float cable_damping = 0.06F;
    float cable_slack = 1.18F;
    int cable_iterations = 4;
    float cable_thickness = 3.0F;

    // Local API (api/local_api.h): off by default, like the web's
    // window.nanoTracker gate. The token is generated on first enable
    // (api::generate_token) and shown in the LOCAL API window.
    bool local_api_enabled = false;
    int local_api_port = 9311; // the web relay's conventional port
    std::string local_api_token;
};

// Loads settings from `path`. A missing or unreadable file yields
// defaults; a malformed file yields defaults (the error is reported on
// stderr, never fatal — settings must not brick startup).
Settings load_settings(const std::filesystem::path& path);

// Writes settings as pretty-printed JSON. Returns false on I/O failure.
bool save_settings(const Settings& settings, const std::filesystem::path& path);

// Conventional location: config_dir()/settings.json.
std::filesystem::path default_settings_path();

} // namespace nt::io
