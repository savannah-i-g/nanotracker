// ui/midi_view — MIDI control surface: input/output device pickers,
// the 24 PPQN clock toggle, live note entry (inbound notes audition
// the selected instrument slot), MIDI-learn arming and the mapping
// list. Owns the input drain: called once per frame, it routes note
// events to the preview path and CCs through the learn/mapping store.
#pragma once

#include "app/project_session.h"
#include "midi/midi_io.h"
#include "midi/midi_learn.h"
#include "midi/midi_out_thread.h"
#include "ui/theme.h"

namespace nt::ui {

class MidiView {
public:
    MidiView(midi::MidiInput& input, midi::MidiOutputPort& output, midi::MidiOutThread& out_thread,
             midi::MidiLearn& learn);

    void draw(app::ProjectSession& session, const Theme& theme);

private:
    void drain_input(app::ProjectSession& session);

    // App-owned devices/threads the view renders for; rebinding is
    // meaningless, so references are the honest shape.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    midi::MidiInput& input_;
    midi::MidiOutputPort& output_;
    midi::MidiOutThread& out_thread_;
    midi::MidiLearn& learn_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    int live_instrument_ = 1;
    int last_note_ = -1;
    int learn_target_ = 0;
};

} // namespace nt::ui
