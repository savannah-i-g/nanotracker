// platform/paths — filesystem locations for config and bundled assets.
// The only module that knows about OS directory conventions.
#pragma once

#include <filesystem>

namespace nt::platform {

// User configuration directory (created on first call):
// $XDG_CONFIG_HOME/nanotracker (falling back to ~/.config/nanotracker)
// on Linux; %APPDATA%\nanotracker on Windows.
std::filesystem::path config_dir();

// Resolves a bundled asset by relative path (e.g. "fonts/KodeMono-Regular.ttf").
// Search order: $NANOTRACKER_ASSETS, the executable's directory, its
// parent (build trees live one level below the project root), then the
// current working directory — each with an "assets/" suffix. Throws
// std::runtime_error if the asset cannot be found.
std::filesystem::path find_asset(const std::filesystem::path& relative);

} // namespace nt::platform
