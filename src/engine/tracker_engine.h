// engine/tracker_engine — the pure sequencer state machine.
// No audio, no I/O, standard library only; advance_tick is called from
// the audio thread at tick boundaries and must stay allocation-free.
// Behavioural reference: Source/.../src/lib/trackerEngine.ts
// advanceTick() — every effect-command semantic here is verified
// against golden tick traces dumped from that implementation
// (tests/engine_golden.cpp).
#pragma once

#include "engine/tracker_types.h"

namespace nt::engine {

// Amiga period for a tracker note (1-96); 0 for empty/invalid.
int note_to_period(int tracker_note);

// Playback-rate ratio for a period given the sample's MIDI base note:
// ratio 1.0 at the base note, 2^(1/12) per semitone up.
double period_to_playback_rate(int period, int sample_base_note);

// Ordered instrument-table slots (1-based) bound to a channel.
// Writes at most `capacity` entries into `out`; returns the count.
int track_bound_instruments(const TrackerProject& project, int channel_index, int* out,
                            int capacity);

// Resolve a cell's bound-instrument sub-slot to a table slot (1-based);
// 0 when the channel has no binding at that index.
int resolve_bound_instrument(const TrackerProject& project, int channel_index, int bound_index);

// Factory helpers (edit-side; allocate freely).
TrackerPattern create_pattern(int id, int row_count, int channel_count);
TrackerProject create_project(int channels = 4);
TrackerPlayState create_play_state(const TrackerProject& project);

// One tick of the sequencer. Tick interval = 2500/bpm ms; `speed` ticks
// per row. Mutates `state` in place: trigger/override flags are
// per-tick outputs, effect memory persists across ticks. Never
// allocates (sequence triggers use the fixed array in the state).
void advance_tick(TrackerPlayState& state, const TrackerProject& project);

// Display helpers (edit-side). `out` must hold at least 4 bytes; the
// note renders as "C-4" / "---" / "===" plus terminator.
const char* note_name(int note, char* out);
int name_to_note(const char* name);

} // namespace nt::engine
