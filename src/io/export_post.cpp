#include "io/export_post.h"

#include <algorithm>
#include <cmath>
#include <ebur128.h>
#include <numbers>

namespace nt::io {

namespace {

// Below this linear amplitude the buffer counts as silence and
// normalization is skipped (matches the web post-processor's floor).
constexpr double kSilenceFloor = 1e-9;

double db_to_linear(double db) {
    return std::pow(10.0, db / 20.0);
}

// Envelope value for normalized position t in [0, 1) (0 = silent end).
double fade_gain(FadeShape shape, double t) {
    return shape == FadeShape::kEqualPower ? std::sin(t * std::numbers::pi / 2.0) : t;
}

void apply_fades(std::vector<float>& interleaved, std::uint32_t rate,
                 const ExportPostOptions& options) {
    const std::size_t frames = interleaved.size() / 2;
    const auto frames_of = [frames, rate](double seconds) {
        return std::min(frames,
                        static_cast<std::size_t>(std::llround(std::max(0.0, seconds * rate))));
    };

    const std::size_t fade_in = frames_of(options.fade_in_seconds);
    for (std::size_t i = 0; i < fade_in; ++i) {
        const auto g = static_cast<float>(fade_gain(
            options.fade_in_shape, static_cast<double>(i) / static_cast<double>(fade_in)));
        interleaved[i * 2] *= g;
        interleaved[(i * 2) + 1] *= g;
    }

    // Mirrors the fade-in grid so the final sample lands exactly at 0.
    const std::size_t fade_out = frames_of(options.fade_out_seconds);
    for (std::size_t i = 0; i < fade_out; ++i) {
        const std::size_t frame = frames - fade_out + i;
        const auto g = static_cast<float>(
            fade_gain(options.fade_out_shape,
                      static_cast<double>(fade_out - i - 1) / static_cast<double>(fade_out)));
        interleaved[frame * 2] *= g;
        interleaved[(frame * 2) + 1] *= g;
    }
}

} // namespace

bool measure_loudness(const float* interleaved_stereo, std::size_t frames, std::uint32_t rate,
                      LoudnessMeasurement& out, std::string& error) {
    // TRUE_PEAK implies SAMPLE_PEAK, so one state serves all three modes.
    ebur128_state* state = ebur128_init(2, rate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    if (state == nullptr) {
        error = "libebur128 init failed";
        return false;
    }
    if (frames > 0 &&
        ebur128_add_frames_float(state, interleaved_stereo, frames) != EBUR128_SUCCESS) {
        ebur128_destroy(&state);
        error = "libebur128 feed failed";
        return false;
    }
    double lufs = -HUGE_VAL;
    ebur128_loudness_global(state, &lufs);
    out.integrated_lufs = lufs;
    out.has_integrated = std::isfinite(lufs);
    out.sample_peak = 0.0;
    out.true_peak = 0.0;
    for (unsigned channel = 0; channel < 2; ++channel) {
        double peak = 0.0;
        if (ebur128_sample_peak(state, channel, &peak) == EBUR128_SUCCESS) {
            out.sample_peak = std::max(out.sample_peak, peak);
        }
        if (ebur128_true_peak(state, channel, &peak) == EBUR128_SUCCESS) {
            out.true_peak = std::max(out.true_peak, peak);
        }
    }
    ebur128_destroy(&state);
    return true;
}

bool apply_export_post(std::vector<float>& interleaved_stereo, std::uint32_t rate,
                       const ExportPostOptions& options, std::string& error) {
    apply_fades(interleaved_stereo, rate, options);

    if (options.normalize == NormalizeMode::kNone || interleaved_stereo.empty()) {
        return true;
    }
    LoudnessMeasurement measured;
    if (!measure_loudness(interleaved_stereo.data(), interleaved_stereo.size() / 2, rate, measured,
                          error)) {
        return false;
    }

    double gain = 1.0;
    switch (options.normalize) {
    case NormalizeMode::kPeak:
        if (measured.sample_peak < kSilenceFloor) {
            return true; // silence: nothing to scale toward the target
        }
        gain = db_to_linear(options.normalize_target_db) / measured.sample_peak;
        break;
    case NormalizeMode::kTruePeak:
        if (measured.true_peak < kSilenceFloor) {
            return true;
        }
        gain = db_to_linear(options.normalize_target_db) / measured.true_peak;
        break;
    case NormalizeMode::kLufs:
        if (!measured.has_integrated) {
            return true; // gated to nothing: no honest gain exists
        }
        gain = db_to_linear(options.normalize_target_db - measured.integrated_lufs);
        break;
    case NormalizeMode::kNone:
        break;
    }
    for (float& sample : interleaved_stereo) {
        sample = static_cast<float>(sample * gain);
    }
    return true;
}

} // namespace nt::io
