// ui/note_entry_view — the note-entry feedback panel: the HUD that was
// missing when typing notes into the pattern grid. It surfaces the
// shared entry state the grid and MIDI step entry both read (current
// octave, edit step, default volume, entry slot), reads back the last
// note written and the active MIDI input mode, and offers an on-screen
// piano keyboard whose clicks enter notes through the exact keyboard
// path (app::enter_note_cell at the pattern cursor, then step advance
// and audition) — a key click IS a keystroke.
//
// The panel holds no entry state of its own: it edits ui::PatternView's
// members through its accessors, so the grid and the panel can never
// disagree about the octave a note lands on.
#pragma once

#include "app/midi_record.h"
#include "app/project_session.h"
#include "ui/pattern_view.h"
#include "ui/theme.h"

namespace nt::ui {

class NoteEntryView {
public:
    // `pattern` owns the canonical entry state and the step-entry
    // cursor; `record` supplies the active MIDI input mode shown in the
    // entry-mode indicator.
    void draw(app::ProjectSession& session, PatternView& pattern, const app::MidiRecord& record,
              const Theme& theme);

private:
    int keyboard_note_down_ = -1; // held key, for the audition release
    int keyboard_octaves_ = 3;    // on-screen span (2..3 octaves)
};

} // namespace nt::ui
