// audio/dsp — small header-only DSP primitives for the FX chains and
// master section. Everything is block-oriented, allocation-free after
// construction, and safe on the audio thread.
// Behavioural references: the web FX modules
// (Source/.../src/components/trackerFxModules/*) and the bitcrusher
// worklet (public/audioworklets/bitcrusher.js).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace nt::audio::dsp {

// One-parameter smoother matching WebAudio's setTargetAtTime feel.
class Smoothed {
public:
    void configure(float value, std::uint32_t rate, float tau_seconds = 0.02F) {
        value_ = target_ = value;
        coeff_ = std::exp(-1.0F / (tau_seconds * static_cast<float>(rate)));
    }

    void set_target(float target) { target_ = target; }

    float next() {
        value_ = target_ + ((value_ - target_) * coeff_);
        return value_;
    }

    [[nodiscard]] float current() const { return value_; }

private:
    float value_ = 0.0F;
    float target_ = 0.0F;
    float coeff_ = 0.0F;
};

// DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1].
class DcBlocker {
public:
    float process(float x) {
        const float y = x - x1_ + (0.995F * y1_);
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float x1_ = 0.0F;
    float y1_ = 0.0F;
};

// RBJ biquad (lowpass/highpass/bandpass), Direct Form 1.
class Biquad {
public:
    enum class Type : std::uint8_t { kLowpass, kHighpass, kBandpass, kNotch };

    void configure(Type type, float freq, float q, std::uint32_t rate) {
        const float w0 = 2.0F * std::numbers::pi_v<float> * (freq / static_cast<float>(rate));
        const float cw = std::cos(w0);
        const float sw = std::sin(w0);
        const float alpha = sw / (2.0F * std::max(0.01F, q));
        float b0 = 1;
        float b1 = 0;
        float b2 = 0;
        switch (type) {
        case Type::kLowpass:
            b0 = (1 - cw) / 2;
            b1 = 1 - cw;
            b2 = (1 - cw) / 2;
            break;
        case Type::kHighpass:
            b0 = (1 + cw) / 2;
            b1 = -(1 + cw);
            b2 = (1 + cw) / 2;
            break;
        case Type::kBandpass:
            b0 = alpha;
            b1 = 0;
            b2 = -alpha;
            break;
        case Type::kNotch:
            b0 = 1;
            b1 = -2 * cw;
            b2 = 1;
            break;
        }
        const float a0 = 1 + alpha;
        b0_ = b0 / a0;
        b1_ = b1 / a0;
        b2_ = b2 / a0;
        a1_ = (-2 * cw) / a0;
        a2_ = (1 - alpha) / a0;
    }

    float process(float x) {
        const float y = (b0_ * x) + (b1_ * x1_) + (b2_ * x2_) - (a1_ * y1_) - (a2_ * y2_);
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        return y;
    }

private:
    float b0_ = 1;
    float b1_ = 0;
    float b2_ = 0;
    float a1_ = 0;
    float a2_ = 0;
    float x1_ = 0;
    float x2_ = 0;
    float y1_ = 0;
    float y2_ = 0;
};

// Fractional-read delay line (max length fixed at construction).
class DelayLine {
public:
    void configure(std::uint32_t max_frames) {
        buffer_.assign(max_frames, 0.0F);
        write_ = 0;
    }

    void write(float x) {
        buffer_[write_] = x;
        write_ = (write_ + 1) % buffer_.size();
    }

    [[nodiscard]] float read(float delay_frames) const {
        const float clamped =
            std::clamp(delay_frames, 1.0F, static_cast<float>(buffer_.size() - 2));
        const float pos = static_cast<float>(write_) + static_cast<float>(buffer_.size()) - clamped;
        const auto idx = static_cast<std::size_t>(pos);
        const float frac = pos - static_cast<float>(idx);
        const float s0 = buffer_[idx % buffer_.size()];
        const float s1 = buffer_[(idx + 1) % buffer_.size()];
        return s0 + ((s1 - s0) * frac);
    }

private:
    std::vector<float> buffer_;
    std::size_t write_ = 0;
};

// The bitcrusher worklet's exact algorithm: phase-accumulated sample
// hold + round-to-nearest bit quantisation.
class Bitcrush {
public:
    void configure(std::uint32_t host_rate) { host_rate_ = host_rate; }

    void set(float bits, float target_rate) {
        step_ = std::pow(2.0F, std::round(std::clamp(bits, 2.0F, 16.0F)));
        phase_step_ = std::clamp(target_rate, 1000.0F, 96000.0F) / static_cast<float>(host_rate_);
    }

    float process(float x) {
        phase_ += phase_step_;
        if (phase_ >= 1.0F) {
            phase_ -= std::floor(phase_);
            last_ = std::floor((x * step_) + 0.5F) / step_;
        }
        return last_;
    }

private:
    std::uint32_t host_rate_ = 48000;
    float step_ = 256.0F;
    float phase_step_ = 0.5F;
    float phase_ = 0.0F;
    float last_ = 0.0F;
};

// Feed-forward compressor in the dB domain approximating WebAudio's
// DynamicsCompressor controls (threshold/ratio/knee/attack/release).
class Compressor {
public:
    void configure(std::uint32_t rate) { rate_ = rate; }

    void set(float threshold_db, float ratio, float attack_s, float release_s, float knee_db) {
        threshold_ = threshold_db;
        ratio_ = std::max(1.0F, ratio);
        knee_ = std::max(0.0F, knee_db);
        attack_coeff_ = std::exp(-1.0F / (std::max(0.001F, attack_s) * static_cast<float>(rate_)));
        release_coeff_ = std::exp(-1.0F / (std::max(0.01F, release_s) * static_cast<float>(rate_)));
    }

    // Stereo-linked gain from the max of both channel magnitudes.
    float gain_for(float l, float r) {
        const float mag = std::max(std::abs(l), std::abs(r));
        const float level_db = 20.0F * std::log10(std::max(1e-6F, mag));
        float over = 0.0F;
        const float half_knee = knee_ / 2.0F;
        if (level_db > threshold_ + half_knee) {
            over = level_db - threshold_;
        } else if (knee_ > 0.0F && level_db > threshold_ - half_knee) {
            const float t = level_db - (threshold_ - half_knee);
            over = (t * t) / (2.0F * knee_);
        }
        const float target_reduction = over - (over / ratio_);
        const float coeff = target_reduction > reduction_ ? attack_coeff_ : release_coeff_;
        reduction_ = target_reduction + ((reduction_ - target_reduction) * coeff);
        return std::pow(10.0F, -reduction_ / 20.0F);
    }

private:
    std::uint32_t rate_ = 48000;
    float threshold_ = -18.0F;
    float ratio_ = 4.0F;
    float knee_ = 3.0F;
    float attack_coeff_ = 0.99F;
    float release_coeff_ = 0.999F;
    float reduction_ = 0.0F;
};

// Freeverb-style comb + allpass bank for the algorithmic reverb.
class Comb {
public:
    void configure(std::uint32_t frames) {
        buffer_.assign(std::max<std::uint32_t>(1, frames), 0.0F);
        pos_ = 0;
        store_ = 0.0F;
    }

    void set(float feedback, float damp) {
        feedback_ = feedback;
        damp_ = damp;
    }

    float process(float x) {
        const float out = buffer_[pos_];
        store_ = (out * (1.0F - damp_)) + (store_ * damp_);
        buffer_[pos_] = x + (store_ * feedback_);
        pos_ = (pos_ + 1) % buffer_.size();
        return out;
    }

private:
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
    float store_ = 0.0F;
    float feedback_ = 0.8F;
    float damp_ = 0.2F;
};

class Allpass {
public:
    void configure(std::uint32_t frames) {
        buffer_.assign(std::max<std::uint32_t>(1, frames), 0.0F);
        pos_ = 0;
    }

    float process(float x) {
        const float bufout = buffer_[pos_];
        const float out = bufout - x;
        buffer_[pos_] = x + (bufout * 0.5F);
        pos_ = (pos_ + 1) % buffer_.size();
        return out;
    }

private:
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
};

} // namespace nt::audio::dsp
