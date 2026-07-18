// audio/effect_midi — the tracker effect-column → MIDI translator
// feeding the tracker bus's midi outs. Behavioural reference:
// Source/.../src/lib/trackerEffectToMidi.ts (mapEffectToMidi); the
// value math, clamps and change-suppression are ported verbatim.
//
// The web mapper speaks IT effect letters (A=1..Z=26) while both the
// web and native pattern models store MOD-style 0x0-0xF effect
// nibbles — the web never bridged the two (mapEffectToMidi has no
// caller), so the whole layer was dormant there. Fixed here, not
// ported: translate() maps the native nibble onto the mapper's
// letter semantics first (see effect_midi.cpp for the table), then
// runs the exact web math.
//
// Txx (native 0xF param >= 0x20) deliberately emits nothing: tempo
// already flows sequencer → snapshot BPM → the MIDI out thread's
// 24 PPQN clock period (midi_out_thread.h). The web's TrackerMidiEvent
// had a "bpm" kind with no wire representation and no consumer; a
// real MIDI cable communicates tempo as clock rate.
//
// Pure functions over caller-owned state; audio-thread safe (no
// allocation).
#pragma once

#include "audio/midi_event.h"

namespace nt::audio {

// Running carrier state converting delta effects into absolute CC /
// pitch-bend values — web PerChannelMidiState with its
// makePerChannelState defaults. One per tracker channel, living as
// long as the playback bundle (reset on kSetBundle).
struct EffectMidiState {
    int last_vol_cc = 100;   // CC 7, 0..127
    int last_pan_cc = 64;    // CC 10, 0..127
    int last_pitch_bend = 0; // -8192..8191
};

// Translates one row's effect column (native MOD nibble + param) into
// zero or more events appended to `list` at `frame`, stamped with
// `channel` (callers pass tracker channel & 0x0F, the web's stamp).
// Mutates `state` so subsequent deltas compound, exactly like the web
// mapper. Unmapped effects append nothing.
void effect_to_midi(int effect, int param, std::uint8_t channel, std::uint32_t frame,
                    EffectMidiState& state, MidiEventList& list);

} // namespace nt::audio
