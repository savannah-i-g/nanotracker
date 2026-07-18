#include "audio/onset_detect.h"

#include "audio/dsp_fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace nt::audio {

namespace {

// ── Analysis geometry ────────────────────────────────────────────────
// The window is fixed in *time*, not frames: ~21 ms (1024 frames at
// 48 kHz), rounded to a power of two at the actual rate so pffft accepts
// it and time resolution stays constant across sample rates. Hop is half
// the window (50 % overlap); with a Hann window this is close enough to
// COLA that a sustained tone's magnitude spectrum is near-identical
// between hops, which is what drives its flux to zero.
constexpr double kWindowSeconds = 1024.0 / 48000.0; // ≈ 21.3 ms
constexpr std::uint32_t kMinWindow = 256;           // ≥ pffft's real minimum
constexpr std::uint32_t kMaxWindow = 4096;          // cap cost at very high rates

// ── Detection function ───────────────────────────────────────────────
// Threshold at frame t is mean + k·std of the flux over a local window
// of ±kThresholdHalfSeconds. A local (not global) threshold tracks
// dynamics — a quiet passage's small onsets survive next to a loud one.
constexpr double kThresholdHalfSeconds = 0.10; // ± window for the moving stats
constexpr float kThresholdK = 1.6F;            // std multiplier over the local mean

// Amplitude-relative floor: a peak must also raise the summed magnitude
// by at least this fraction of the mean per-frame magnitude. Being a
// ratio of two amplitude-proportional quantities, it is level-invariant
// (a −6 dB copy scales flux and magnitude alike), yet it discards the
// numerical flux of a steady tone, whose mean magnitude dwarfs its
// flux — so a pure sine yields no onsets rather than one spurious peak.
constexpr float kFluxMagnitudeFloor = 0.15F;

// A transient smears across a couple of hops; requiring this gap between
// accepted onsets stops one hit registering twice. 30 ms is below the
// spacing of even fast drum rolls yet above a single transient's width.
constexpr double kMinOnsetGapSeconds = 0.030;

// Below this, the whole buffer's peak flux is treated as numerical dust
// (true silence or DC): the detector reports no transients. Real audio
// at any usable level sits many orders of magnitude above it, so the
// guard never rejects a quiet-but-real signal.
constexpr float kSilenceFluxEpsilon = 1e-9F;

[[nodiscard]] std::uint32_t next_pow2(std::uint32_t v) {
    std::uint32_t p = 1;
    while (p < v) {
        p <<= 1U;
    }
    return p;
}

// Nearest power of two in log space (halfway rounds up), clamped to the
// valid window range.
[[nodiscard]] std::uint32_t nearest_pow2_window(double target) {
    const auto hi = next_pow2(static_cast<std::uint32_t>(std::ceil(target)));
    const std::uint32_t lo = hi >> 1U;
    const std::uint32_t chosen =
        (lo >= kMinWindow && (target * target) < static_cast<double>(lo) * hi) ? lo : hi;
    return std::clamp(chosen, kMinWindow, kMaxWindow);
}

} // namespace

OnsetAnalysis onset_analysis_for(std::uint32_t rate) {
    OnsetAnalysis an;
    an.window = nearest_pow2_window(kWindowSeconds * rate);
    an.hop = an.window / 2;
    return an;
}

std::vector<std::uint32_t> detect_onsets(const float* mono, std::size_t frames, std::uint32_t rate,
                                         std::uint32_t max_onsets) {
    if (mono == nullptr || rate == 0 || max_onsets == 0) {
        return {};
    }
    const OnsetAnalysis an = onset_analysis_for(rate);
    const std::uint32_t window = an.window;
    const std::uint32_t hop = an.hop;
    if (frames < window) {
        return {}; // shorter than one window — nothing to analyse
    }
    const std::size_t frame_count = ((frames - window) / hop) + 1;
    if (frame_count < 2) {
        return {}; // a flux value needs two consecutive frames
    }

    // Hann window: tapered edges keep a transient from ringing across the
    // whole spectrum and give the phase-invariance the flux relies on.
    std::vector<float> hann(window);
    for (std::uint32_t n = 0; n < window; ++n) {
        hann[n] = 0.5F - (0.5F * std::cos(2.0F * std::numbers::pi_v<float> * static_cast<float>(n) /
                                          static_cast<float>(window - 1)));
    }

    dsp::RealFft fft(window);
    dsp::FftBuffer frame(window);     // windowed samples
    dsp::FftBuffer frame_rev(window); // circular reversal ⇒ conj spectrum
    dsp::FftBuffer spec(window);
    dsp::FftBuffer spec_rev(window);
    dsp::FftBuffer power(window); // X·conj(X) == |X|² per bin (all real)
    std::vector<float> mag_prev(window, 0.0F);
    std::vector<float> mag_cur(window, 0.0F);
    std::vector<float> flux(frame_count, 0.0F);
    double magnitude_sum = 0.0; // Σ over frames of Σ|X[k]|, for the floor

    for (std::size_t fi = 0; fi < frame_count; ++fi) {
        const std::size_t start = fi * hop;
        for (std::uint32_t n = 0; n < window; ++n) {
            const std::size_t src = start + n;
            frame.data()[n] = src < frames ? mono[src] * hann[n] : 0.0F;
        }
        // conj(X[k]) is the transform of x[(-n) mod N]: y[0]=x[0],
        // y[n]=x[N-n]. Then X·conj(X) is the real power spectrum, and
        // convolve_accumulate does that per-bin product in whatever
        // internal layout the plan uses — the sum below is layout-blind.
        frame_rev.data()[0] = frame.data()[0];
        for (std::uint32_t n = 1; n < window; ++n) {
            frame_rev.data()[n] = frame.data()[window - n];
        }
        fft.forward(frame.data(), spec.data());
        fft.forward(frame_rev.data(), spec_rev.data());
        power.zero();
        fft.convolve_accumulate(spec.data(), spec_rev.data(), power.data(), 1.0F);

        double frame_magnitude = 0.0;
        for (std::uint32_t j = 0; j < window; ++j) {
            const float mag = std::sqrt(std::max(0.0F, power.data()[j]));
            mag_cur[j] = mag;
            frame_magnitude += mag;
        }
        magnitude_sum += frame_magnitude;

        if (fi > 0) {
            double positive = 0.0;
            for (std::uint32_t j = 0; j < window; ++j) {
                const float delta = mag_cur[j] - mag_prev[j];
                if (delta > 0.0F) {
                    positive += delta;
                }
            }
            flux[fi] = static_cast<float>(positive);
        }
        std::swap(mag_prev, mag_cur);
    }

    const float peak_flux = *std::max_element(flux.begin(), flux.end());
    if (peak_flux <= kSilenceFluxEpsilon) {
        return {}; // silence or DC
    }
    const float magnitude_floor =
        kFluxMagnitudeFloor * static_cast<float>(magnitude_sum / static_cast<double>(frame_count));

    const auto half =
        static_cast<std::size_t>(std::max(1.0, std::round(kThresholdHalfSeconds * rate / hop)));
    const auto gap =
        static_cast<std::size_t>(std::max(1.0, std::round(kMinOnsetGapSeconds * rate / hop)));

    // Peak-pick: a strict local maximum that clears both the adaptive
    // threshold and the magnitude floor, no closer than `gap` frames to
    // the previous accepted onset. `last` starts far enough back that
    // the first eligible frame is never gap-suppressed.
    std::vector<std::pair<std::size_t, float>> peaks; // (frame index, flux)
    std::size_t last = 0;
    bool have_last = false;
    for (std::size_t t = 1; t + 1 < frame_count; ++t) {
        if (flux[t] < magnitude_floor || flux[t] <= flux[t - 1] || flux[t] < flux[t + 1]) {
            continue;
        }
        const std::size_t lo = t > half ? t - half : 0;
        const std::size_t hi = std::min(frame_count - 1, t + half);
        double sum = 0.0;
        double sq = 0.0;
        const auto count = static_cast<double>(hi - lo + 1);
        for (std::size_t i = lo; i <= hi; ++i) {
            sum += flux[i];
            sq += static_cast<double>(flux[i]) * flux[i];
        }
        const double mean = sum / count;
        const double variance = std::max(0.0, (sq / count) - (mean * mean));
        const double threshold = mean + (kThresholdK * std::sqrt(variance));
        if (flux[t] <= threshold) {
            continue;
        }
        if (have_last && t - last < gap) {
            continue;
        }
        peaks.emplace_back(t, flux[t]);
        last = t;
        have_last = true;
    }

    // Cap by strength: keep the loudest `max_onsets`, breaking ties in
    // favour of the earlier frame so the selection is a total order.
    if (peaks.size() > max_onsets) {
        std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        peaks.resize(max_onsets);
    }

    // Report the analysis frame's centre, not its start: the flux at
    // frame t measures energy that entered as the window advanced onto
    // the transient, so the event sits ~half a window past t·hop.
    // Centring removes that bias and lands the offset within a hop of
    // the true transient. Clamp to the buffer so a late frame's centre
    // never points past the end.
    std::vector<std::uint32_t> offsets;
    offsets.reserve(peaks.size());
    const auto last_frame = static_cast<std::uint32_t>(frames - 1);
    for (const auto& [t, unused] : peaks) {
        offsets.push_back(
            std::min(last_frame, static_cast<std::uint32_t>((t * hop) + (window / 2))));
    }
    std::sort(offsets.begin(), offsets.end());
    offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    return offsets;
}

} // namespace nt::audio
