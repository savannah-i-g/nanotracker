// ui/fx_mixer_view — the FX MIXER window: channel strips with module
// stacks, parameters, and per-tracker-channel sends (web reference:
// TrackerFxMixerPanel.tsx). Structural edits rebuild the audio rack;
// parameter drags reach the live rack via command.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

namespace nt::ui {

class FxMixerView {
public:
    static void draw(app::ProjectSession& session, const Theme& theme);
};

} // namespace nt::ui
