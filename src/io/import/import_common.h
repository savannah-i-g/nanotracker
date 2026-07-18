// io/import/import_common — shared helpers for the module importers
// (behavioural reference: Source/.../src/lib/importerUtils.ts).
// Import converts foreign modules into NanoTracker's editing model and
// is inherently lossy; every anomaly funnels into ImportResults so the
// import dialog can show exactly what happened (fix-don't-retain #2:
// nothing is dropped silently). Faithful playback is the Module
// Player's job, not import's.
#pragma once

#include "engine/tracker_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nt::io::import {

struct ImportResults {
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    std::vector<std::string> info;
};

// Null-terminated, trailing-space-trimmed ASCII field.
std::string read_string(const std::uint8_t* data, std::size_t len);

// Nearest tracker note (1-96) for an Amiga period; 0 for period 0.
int period_to_note(int period);

int clamp_channels(int n);

// Amiga timing constants (PAL clock; period 428 = MOD C-2).
inline constexpr double kAmigaClockPal = 7093789.2;
inline constexpr int kModSampleRate = 8287; // round(clock / (428*2))
inline constexpr int kModTargetRate = 44100;
inline constexpr int kModBaseNote = 84; // MIDI C-6 = tracker note 73

// Wraps mono 16-bit PCM in a WAV container (imported samples store WAV
// bytes in TrackerSample::original_data for FTRK compatibility).
std::vector<std::uint8_t> build_wav16(const std::vector<std::int16_t>& pcm,
                                      std::uint32_t sample_rate);

// Converts float [-1,1] mono to a 16-bit WAV at the given rate.
std::vector<std::uint8_t> build_wav_from_float(const std::vector<float>& pcm,
                                               std::uint32_t sample_rate);

// MOD path: linear-interpolation upsample of signed 8-bit PCM from
// kModSampleRate to kModTargetRate, as 16-bit WAV. Returns the output
// frame count for loop-point scaling.
struct ModWav {
    std::vector<std::uint8_t> wav;
    std::uint32_t out_frames = 0;
};

ModWav build_wav_mod(const std::int8_t* pcm, std::size_t length);

} // namespace nt::io::import
