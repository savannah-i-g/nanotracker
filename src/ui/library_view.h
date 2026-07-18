// ui/library_view — the LIBRARY window: a browser over user-declared
// library root directories. Roots are settings-persisted and added by
// path (no OS dialog in-tree, matching the rest of the app). Under the
// roots it lists two asset kinds: samples (.wav/.ogg/.mp3, auditioned
// through ProjectSession::preview_file) and NTP archives
// (.ntins/.ntsfx, "installed" via ProjectSession::load_plugin_file into
// the catalogue). Favourited paths surface in a section at the top.
//
// Scope guard (owner's plan): a browser over real directories — no
// database, no tagging beyond favourites. The recursive listing is
// bounded (depth + entry caps) and cached per root-set; it is rebuilt on
// a rescan or a root change, never walked every frame. Presets are out
// of scope here — they live per-plugin in the config directory and are
// applied from the plugin panel, not as loose library files.
//
// The extension classifier and favourites toggle are unit-tested; the
// window is screenshot-verified.
#pragma once

#include "app/project_session.h"
#include "io/settings.h"
#include "ui/theme.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::ui {

enum class AssetKind : std::uint8_t {
    kOther,      // ignored by the browser
    kSample,     // .wav / .ogg / .mp3
    kNtpArchive, // .ntins / .ntsfx
};

// The classifier and favourites toggle are inline (header-defined) so
// the test binary can exercise them without linking the UI/imgui layer.

// Classifies a file by its (case-insensitive) extension. Directories and
// unrelated files map to kOther.
[[nodiscard]] inline AssetKind classify_asset(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") {
        return AssetKind::kSample;
    }
    if (ext == ".ntins" || ext == ".ntsfx") {
        return AssetKind::kNtpArchive;
    }
    return AssetKind::kOther;
}

// Adds `path` to `favourites` if absent, removes it if present. Returns
// the resulting favourited state.
inline bool toggle_favourite(std::vector<std::string>& favourites, const std::string& path) {
    const auto it = std::find(favourites.begin(), favourites.end(), path);
    if (it != favourites.end()) {
        favourites.erase(it);
        return false;
    }
    favourites.push_back(path);
    return true;
}

class LibraryView {
public:
    // `open` is the VIEW-menu visibility flag; the window's close box
    // clears it (the DEBUG/HISTORY window contract).
    void draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme, bool& open);

private:
    struct Asset {
        std::filesystem::path path;
        std::string label; // path relative to its root, for display
        AssetKind kind = AssetKind::kOther;
        std::uintmax_t bytes = 0;
    };

    // Rebuilds the cached listing across every root (bounded walk).
    void rescan(const io::Settings& settings);
    void audition(app::ProjectSession& session, const std::filesystem::path& path);
    void install(app::ProjectSession& session, const std::filesystem::path& path);
    // Row control shared by the favourites section and the listing:
    // star toggle + name + kind-appropriate action button.
    void draw_asset_row(app::ProjectSession& session, io::Settings& settings, const Theme& theme,
                        const std::filesystem::path& path, const std::string& label,
                        AssetKind kind);

    std::vector<Asset> assets_;        // cached recursive listing across roots
    bool dirty_ = true;                // listing needs a rescan
    std::size_t roots_signature_ = 0;  // detects a root-set change → rescan
    std::array<char, 512> new_root_{}; // add-root text field
    std::string status_;
};

} // namespace nt::ui
