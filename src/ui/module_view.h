// ui/module_view — the Module Player window: load a tracker module
// (MOD/XM/S3M/IT and the rest of libopenmpt's catalogue), play it
// faithfully, watch position. The player is an audio source the
// engine mixes; it becomes a patch-graph node in the workspace stage.
#pragma once

#include "audio/audio_engine.h"
#include "modplay/module_player.h"
#include "ui/theme.h"

#include <array>
#include <string>

namespace nt::ui {

class ModuleView {
public:
    // Draws the window. `player` is app-owned; attach/detach commands
    // flow through `audio`.
    void draw(audio::AudioEngine& audio, modplay::ModulePlayer& player, const Theme& theme);

    // Loads a module file and attaches it to the engine (also used by
    // the --play-module automation hook).
    bool load_file(audio::AudioEngine& audio, modplay::ModulePlayer& player,
                   const std::string& path);

private:
    std::array<char, 512> path_buf_{};
    std::string status_;
};

} // namespace nt::ui
