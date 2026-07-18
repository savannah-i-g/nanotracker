// engine/sequence_ops — pure transforms over sequence-note sets.
// Behavioural reference: the web app's lib/sequenceMidiTools.ts. Each
// function maps an input selection to a new note vector; no session,
// engine, or UI state is touched. The caller owns applying results to
// a layer — sequence edits are structural (stop + republish) and the
// outputs are not start-tick-sorted, so re-sort on application.
//
// Except for arpeggiate (which generates a fresh run), every
// transform returns exactly one note per input note, in input order,
// so callers can map results back onto selection indices.
//
// Randomized transforms draw from a caller-seeded std::mt19937 via
// its standard-specified raw output (never the implementation-defined
// std distributions), so a given seed reproduces exactly on every
// platform.
#pragma once

#include "engine/tracker_types.h"

#include <cstdint>
#include <random>
#include <vector>

namespace nt::engine {

enum class ArpDirection : std::uint8_t { kUp, kDown, kUpDown, kRandom };

enum class VelocityCurveShape : std::uint8_t { kCrescendo, kDecrescendo, kAccent, kFlat };

// Snaps note starts to the grid of `speed / division` ticks (division
// 1 = whole rows, 2 = half rows). `strength_percent` blends between
// the original and snapped position (100 = full snap). Durations snap
// too when `quantize_duration` is set, to a minimum of one grid step.
std::vector<SequenceNote> quantize(const std::vector<SequenceNote>& notes, int speed,
                                   int division = 1, int strength_percent = 100,
                                   bool quantize_duration = false);

// Random jitter on timing (± timing_percent of a row), velocity
// (± velocity_amount) and duration (± duration_percent of each note's
// length). Per note the draws are ordered timing, velocity, duration.
std::vector<SequenceNote> humanize(const std::vector<SequenceNote>& notes, int speed,
                                   std::mt19937& rng, int timing_percent = 30,
                                   int velocity_amount = 15, int duration_percent = 20);

// Pitch shift clamped into the MIDI range.
std::vector<SequenceNote> transpose(const std::vector<SequenceNote>& notes, int semitones);

// Time mirror across the selection's span: the block plays back to
// front while each note still sounds forwards.
std::vector<SequenceNote> reverse(const std::vector<SequenceNote>& notes);

// Pitch mirror around the selection's pitch center (midpoint of its
// lowest and highest note), clamped into the MIDI range.
std::vector<SequenceNote> invert(const std::vector<SequenceNote>& notes);

// Replaces the selection with an arpeggiated run over its unique
// pitches (expanded across `octave_range` octaves), stepping every
// `speed / rate` ticks across the selection's time span. Each step
// gates at `gate_percent` of the step length; velocity is the mean of
// the source notes. kRandom shuffles the pitch cycle once using
// `rng`; the other directions ignore it.
std::vector<SequenceNote> arpeggiate(const std::vector<SequenceNote>& notes, int speed,
                                     ArpDirection direction, int rate, std::mt19937& rng,
                                     int gate_percent = 80, int octave_range = 1);

// Shapes velocities across the selection in time order: linear ramp
// between min and max (crescendo/decrescendo), every fourth note
// accented (max, others min), or flat at the midpoint.
std::vector<SequenceNote> velocity_curve(const std::vector<SequenceNote>& notes,
                                         VelocityCurveShape shape, int min_velocity = 30,
                                         int max_velocity = 120);

// Rescales note lengths. At >= 100 each note instead extends to
// `scale_percent` of the gap to the next later-starting note (full
// legato at 100, overlap above), falling back to plain length scaling
// for the block's final chord.
std::vector<SequenceNote> gate_length(const std::vector<SequenceNote>& notes, int scale_percent);

} // namespace nt::engine
