// audio/onset_detect — deterministic spectral-flux onset detection.
//
// Feeds slice-map "transients" auto-detection (plugins/ntp_loader): a
// mono-summed source buffer in, transient frame offsets out, computed
// once at load time off the audio thread. No RNG, no hidden state — the
// same input always yields the same offsets, so a plugin's derived
// slices are reproducible across loads and machines.
//
// Algorithm (details and the constant rationale live in the .cpp):
//   1. STFT with a Hann window, 50 % overlap (hop = window / 2). The
//      window duration is fixed in time (~21 ms) and rounded to a power
//      of two at the actual sample rate, so resolution is rate-stable.
//   2. Per frame, the magnitude spectrum |X[k]|. Magnitude is
//      phase-invariant, so a steady tone (whose bins only rotate in
//      phase between hops) produces ~zero flux — the property that makes
//      spectral flux reject sustained sound and fire on transients.
//   3. Half-wave-rectified spectral flux: the summed positive
//      frame-to-frame magnitude increase.
//   4. Adaptive threshold (local mean + k·std) with an amplitude-
//      relative floor, then peak-pick with a minimum inter-onset gap.
//
// The magnitude spectrum is obtained through the project FFT wrapper's
// public interface only (audio/dsp_fft): |X[k]|² == X[k]·conj(X[k]), and
// conj(X) is the transform of the circularly time-reversed frame, so a
// forward + forward + convolve_accumulate yields the power spectrum
// without ever touching pffft's opaque bin layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nt::audio {

// The STFT geometry the detector uses at a given rate. Exposed so
// callers and tests can reason about onset-offset resolution: a
// returned offset is accurate to within roughly one hop.
struct OnsetAnalysis {
    std::uint32_t window = 0; // STFT window length in frames (power of two)
    std::uint32_t hop = 0;    // hop length in frames (window / 2)
};

[[nodiscard]] OnsetAnalysis onset_analysis_for(std::uint32_t rate);

// Detected transient onsets as frame offsets into `mono`, sorted
// ascending and unique. `mono` is a mono-summed signal of `frames`
// samples at `rate` Hz. At most `max_onsets` offsets are returned; when
// more transients are found, the strongest by flux magnitude are kept
// (deterministic tie-break: earlier frame wins). The result is empty
// when the input carries no transients (silence, DC, or a sustained
// tone) or is shorter than one analysis window — the caller then falls
// back to a single whole-sample slice.
[[nodiscard]] std::vector<std::uint32_t>
detect_onsets(const float* mono, std::size_t frames, std::uint32_t rate, std::uint32_t max_onsets);

} // namespace nt::audio
