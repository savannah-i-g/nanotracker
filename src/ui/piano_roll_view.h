// ui/piano_roll_view — sequence-layer editor. Behavioural reference:
// SequencePianoRollEditor.tsx; the model is the engine's sequence
// mixer (up to 4 layers per channel, notes in ticks — tracker rows are
// `speed` ticks wide). Click adds a note, right-click removes, drag on
// a note's tail resizes; edits route through the session so they are
// undoable and the sequencer picks them up live (fixed-size writes,
// same contract as pattern cells).
//
// The on-screen keyboard lives at the bottom of the window: two
// octaves of buttons that audition the selected layer's instrument
// through the preview path (sample or plugin alike).
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

namespace nt::ui {

class PianoRollView {
public:
    void draw(app::ProjectSession& session, const Theme& theme);

private:
    int channel_ = 0;
    int layer_ = 0;
    int base_pitch_ = 56; // bottom row of the grid
    int keyboard_note_down_ = -1;
};

} // namespace nt::ui
