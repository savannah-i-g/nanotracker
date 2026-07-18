// app/midi_record — inbound MIDI note routing for the input modes the
// web exposed in TrackerMidiPanel.tsx (preview | enter | record; the
// Tracker.tsx onNoteOn handler is the behavioural reference). One
// controller consumes note events from BOTH inbound sources — the
// hardware device ring (ui/midi_view's per-frame drain forwards note
// events here) and the cabled master.midi.in ring
// (AudioEngine::poll_bus_midi_in, drained by update()) — so a note
// applies identically however it arrived. Ordering per frame: device
// events first (the MIDI window draws before update() runs), then bus
// events; each source keeps its own FIFO order.
//
// Modes:
//   kOff     — inbound notes are ignored.
//   kPreview — audition only, no pattern writes: sample slots through
//              ProjectSession::preview_note on the armed channel,
//              plugin slots through preview_plugin_note (note-off
//              releases the plugin preview, as in ui/midi_view before
//              this controller absorbed the path).
//   kEnter   — step entry: the note writes the cell at the pattern
//              editor's cursor through the exact keyboard-entry path
//              (enter_note_cell below) and advances the cursor by the
//              keyboard edit step. Works with the transport stopped.
//              The web kept a separate step-row counter
//              (midiEnterRow); the native cursor IS the step row —
//              divergence recorded in Docs/FIXES.md.
//   kRecord  — live record: while the transport plays, the note is
//              quantised to the nearest pattern row (quantise_record_row
//              below) and written on the armed channel. Note-offs
//              never write cells — web parity: onNoteOff only touched
//              preview state, so live record captures note starts only.
//
// Velocity → volume (web velocityToVolume, default on): enter/record
// map velocity to the cell volume column as round(velocity/127*64);
// disabled, the volume column is left untouched — keyboard-entry
// parity; the web cleared it to "default" instead (Docs/FIXES.md).
#pragma once

#include "app/project_session.h"
#include "audio/audio_engine.h"

#include <cstdint>

namespace nt::app {

enum class MidiInputMode : std::uint8_t { kOff, kPreview, kEnter, kRecord };

// Narrow surface of the pattern editor's cursor for step entry — the
// controller writes through the keyboard-entry path without seeing the
// ImGui view. Implemented by ui::PatternView; values mirror its last
// drawn frame.
class PatternCursor {
public:
    PatternCursor() = default;
    PatternCursor(const PatternCursor&) = default;
    PatternCursor& operator=(const PatternCursor&) = default;
    PatternCursor(PatternCursor&&) = default;
    PatternCursor& operator=(PatternCursor&&) = default;
    virtual ~PatternCursor() = default;

    [[nodiscard]] virtual int step_pattern() const = 0;
    [[nodiscard]] virtual int step_row() const = 0;
    [[nodiscard]] virtual int step_channel() const = 0;
    // The instrument slot keyboard entry writes (the SMP selector).
    [[nodiscard]] virtual int step_entry_slot() const = 0;
    // The keyboard edit-step advance (one row, clamped at the last).
    virtual void step_advance() = 0;
    // Records the note just entered for the NOTE ENTRY panel readout —
    // the keyboard path sets PatternView::last_note_ inline, so MIDI
    // step entry keeps the readout in step through here (tracker note
    // 1..96).
    virtual void step_set_last_note(int note) = 0;
};

// The note-entry cell write shared by the pattern editor's keyboard
// path and MIDI enter/record: reads the current cell, sets note and
// instrument with the track-bind convention (bound channels leave
// instrument 0 and resolve at playback), keeps every other column,
// routes through ProjectSession::set_cell (undoable) and auditions the
// note when asked. `volume` -1 keeps the cell's volume column
// (keyboard entry never touches it); 0..64 writes it. Out-of-range
// coordinates are ignored, matching set_cell.
void enter_note_cell(ProjectSession& session, int pattern_index, int row, int channel, int note,
                     int entry_slot, int volume, bool audition);

// Transport anchor lifted from an EngineSnapshot at drain time.
struct RecordAnchor {
    int row = 0;   // playhead row at publish
    int tick = 0;  // ticks elapsed within that row at publish
    int speed = 6; // ticks per row
    int bpm = 125; // classic tempo: one tick = 2.5/bpm seconds
    int pattern_rows = 64;
    std::uint32_t sample_rate = 48000;
    std::uint64_t stream_frames = 0; // total frames rendered at publish
    // Frames elapsed since the (row, tick) edge above, at publish
    // (EngineSnapshot::tick_frame_remainder). stream_frames minus this
    // is the exact frame that edge began — the quantiser anchors there
    // rather than assuming the publish landed on the boundary.
    std::uint32_t tick_frame_remainder = 0;
};

// Maps an event's absolute stream frame onto the pattern row it
// records into. Quantise rule: nearest row, halves round UP — an
// event in the second half of a row lands on the NEXT row, so a note
// played just ahead of a row boundary records onto that boundary row.
// Rounding past the final row wraps to row 0 of the same pattern (the
// single-pattern loop case; across order-list transitions late events
// quantise against the next snapshot, which already reports the new
// pattern). Precision: the anchor recovers the frame its (row, tick)
// edge began (stream_frames - tick_frame_remainder), so a mid-tick
// publish no longer reads early. The web recorded at whatever row the
// UI thread happened to observe, which is strictly coarser. Returns
// the row, clamped/wrapped into [0, pattern_rows); guards (transport
// not running, zero speed/bpm/rate) return the anchor row.
[[nodiscard]] int quantise_record_row(const RecordAnchor& anchor, std::uint64_t at_frame);

class MidiRecord {
public:
    MidiRecord(audio::AudioEngine& audio, PatternCursor& cursor);

    // ── Mode + settings surface (drawn by ui/midi_view) ─────────────
    [[nodiscard]] MidiInputMode mode() const { return mode_; }

    void set_mode(MidiInputMode mode) { mode_ = mode; }

    [[nodiscard]] int armed_channel() const { return armed_channel_; }

    void set_armed_channel(int channel) { armed_channel_ = channel; }

    [[nodiscard]] bool velocity_to_volume() const { return velocity_to_volume_; }

    void set_velocity_to_volume(bool enabled) { velocity_to_volume_ = enabled; }

    [[nodiscard]] int preview_slot() const { return preview_slot_; }

    void set_preview_slot(int slot) { preview_slot_ = slot; }

    // ── Event entry points ──────────────────────────────────────────
    // Device-ring notes (ui/midi_view forwards them unless midi-learn
    // is armed — learn wins; see the drain). Untimed, so they are
    // stamped "now": the latest snapshot's stream_frames.
    void on_device_note_on(ProjectSession& session, int midi_note, int velocity);
    void on_device_note_off(ProjectSession& session, int midi_note);

    // Once per frame: drains the cabled master.midi.in ring and routes
    // its note events (frame-accurate at_frame stamps) through the
    // same mode dispatch. CC/pitch-bend deliveries are discarded — the
    // web recorded notes only. Drains in every mode so the ring never
    // sits full while input is off.
    void update(ProjectSession& session);

private:
    void apply_note_on(ProjectSession& session, int midi_note, int velocity,
                       std::uint64_t at_frame);
    void apply_note_off(ProjectSession& session, int midi_note);
    void preview(ProjectSession& session, int midi_note, int velocity);
    void record(ProjectSession& session, int midi_note, int velocity, std::uint64_t at_frame);

    // App-owned engine/view surfaces; rebinding is meaningless.
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    audio::AudioEngine& audio_;
    PatternCursor& cursor_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    MidiInputMode mode_ = MidiInputMode::kPreview; // web default
    int armed_channel_ = 0;
    bool velocity_to_volume_ = true; // web default
    int preview_slot_ = 1;
    int last_preview_note_ = -1;
};

} // namespace nt::app
