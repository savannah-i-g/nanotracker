// io/export_post — post-processing applied to the rendered float
// buffer between the offline render and the encoder, in a fixed order:
//
//   1. fade in / fade out (linear or equal-power envelopes)
//   2. normalization (peak / true-peak / LUFS-integrated)
//
// Normalization measures the post-fade signal and applies exactly one
// linear gain — no limiter follows, so a LUFS target hotter than the
// material's crest factor can push peaks past full scale (the caller
// owns that trade-off, as the web app did). Measurement is libebur128
// (ITU-R BS.1770 / EBU R 128, conformance-passing — no
// self-implementation; Docs/Plan_PostV1/Research/05).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt::io {

enum class FadeShape : std::uint8_t { kLinear, kEqualPower };

enum class NormalizeMode : std::uint8_t { kNone, kPeak, kTruePeak, kLufs };

struct ExportPostOptions {
    double fade_in_seconds = 0.0; // 0 = no fade
    double fade_out_seconds = 0.0;
    FadeShape fade_in_shape = FadeShape::kLinear;
    FadeShape fade_out_shape = FadeShape::kLinear;
    NormalizeMode normalize = NormalizeMode::kNone;
    // Target level in the selected mode's unit: dBFS for kPeak, dBTP
    // for kTruePeak, LUFS for kLufs.
    double normalize_target_db = -14.0;
};

// One libebur128 pass over a stereo buffer. Peaks are linear
// amplitudes (max of both channels); integrated loudness is LUFS.
// `has_integrated` is false when gating leaves no blocks (silence or
// material shorter than one 400 ms gating block).
struct LoudnessMeasurement {
    double sample_peak = 0.0;
    double true_peak = 0.0;
    double integrated_lufs = 0.0; // valid only when has_integrated
    bool has_integrated = false;
};

bool measure_loudness(const float* interleaved_stereo, std::size_t frames, std::uint32_t rate,
                      LoudnessMeasurement& out, std::string& error);

// Applies fades then normalization to an interleaved stereo buffer in
// place (the order contract above). Silent or unmeasurable input skips
// normalization rather than failing. Returns false with `error` set
// only on measurement failure.
bool apply_export_post(std::vector<float>& interleaved_stereo, std::uint32_t rate,
                       const ExportPostOptions& options, std::string& error);

} // namespace nt::io
