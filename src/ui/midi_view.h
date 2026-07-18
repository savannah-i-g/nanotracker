// ui/midi_view — MIDI control surface: input/output device pickers,
// the 24 PPQN clock toggle, the inbound-note input mode (off | preview
// | enter | record, applied by app/midi_record), MIDI-learn arming and
// the mapping list. Precedence: while learn is armed the device is
// being wiggled to bind, so note events are dropped — they must not
// preview or write pattern cells until the arm resolves.
#pragma once

#include "app/midi_record.h"
#include "app/project_session.h"
#include "midi/midi_io.h"
#include "midi/midi_learn.h"
#include "midi/midi_out_thread.h"
#include "ui/theme.h"

namespace nt::ui {

class MidiView {
public:
    MidiView(midi::MidiInput& input, midi::MidiOutputPort& output, midi::MidiOutThread& out_thread,
             midi::MidiLearn& learn, app::MidiRecord& record);

    void draw(app::ProjectSession& session, const Theme& theme);

    // Device-ring drain: forwards note events to the record controller
    // and CCs to the learn/mapping store. Called unconditionally once per
    // frame from the main loop — independent of this window's visibility,
    // so hiding the midi window never pauses device intake.
    void drain_input(app::ProjectSession& session);

private:
    void draw_input_mode(app::ProjectSession& session, const Theme& theme);

    // App-owned devices/threads/controllers the view renders for;
    // rebinding is meaningless, so references are the honest shape.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    midi::MidiInput& input_;
    midi::MidiOutputPort& output_;
    midi::MidiOutThread& out_thread_;
    midi::MidiLearn& learn_;
    app::MidiRecord& record_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    int learn_target_ = 0;
};

} // namespace nt::ui
