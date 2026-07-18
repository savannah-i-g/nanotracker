// Partitioned-FFT convolution engine verification (Stage 20): the
// engine must be a drop-in replacement for direct FIR — null tests
// across impulse lengths and call phases, long-impulse correctness,
// determinism, and RT-safety of process().
#include "audio/convolution_engine.h"
#include "rt/rt_assert.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr std::uint32_t kBlock = 128;

// Deterministic xorshift noise — no library RNG, reproducible runs.
std::vector<float> noise(std::uint32_t seed, std::size_t frames) {
    std::vector<float> out(frames);
    std::uint32_t state = seed | 1U;
    for (float& v : out) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        v = (static_cast<float>(state & 0xFFFFFF) / static_cast<float>(0x800000)) - 1.0F;
    }
    return out;
}

// Unit-energy noise impulse: keeps the convolution output at O(1)
// amplitude for every length, so one absolute tolerance (1e-5) reads
// as the same relative accuracy across the length sweep.
std::vector<float> noise_impulse(std::uint32_t seed, std::size_t taps) {
    std::vector<float> impulse = noise(seed, taps);
    double energy = 0.0;
    for (const float v : impulse) {
        energy += static_cast<double>(v) * v;
    }
    const float scale = static_cast<float>(1.0 / std::sqrt(energy));
    for (float& v : impulse) {
        v *= scale;
    }
    return impulse;
}

// Time-domain reference: y[n] = sum_t h[t] * x[n-t].
std::vector<float> direct_fir(const std::vector<float>& impulse, const std::vector<float>& input) {
    std::vector<float> out(input.size(), 0.0F);
    for (std::size_t n = 0; n < input.size(); ++n) {
        double acc = 0.0;
        const std::size_t t_end = std::min(impulse.size(), n + 1);
        for (std::size_t t = 0; t < t_end; ++t) {
            acc += static_cast<double>(impulse[t]) * input[n - t];
        }
        out[n] = static_cast<float>(acc);
    }
    return out;
}

// Runs the engine over `input` in call chunks drawn cyclically from
// `chunks` (so call boundaries sweep across partition boundaries).
std::vector<float> run_engine(nt::audio::ConvolutionEngine& engine, const std::vector<float>& input,
                              const std::vector<std::uint32_t>& chunks) {
    std::vector<float> out(input.size(), 0.0F);
    std::size_t at = 0;
    std::size_t chunk_index = 0;
    while (at < input.size()) {
        const auto want = chunks[chunk_index % chunks.size()];
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(want, input.size() - at));
        engine.process(input.data() + at, out.data() + at, n);
        at += n;
        ++chunk_index;
    }
    return out;
}

float goertzel(const std::vector<float>& mono, std::uint32_t rate, float freq) {
    const std::size_t n = mono.size();
    const double k = std::round(static_cast<double>(n) * freq / rate);
    const double w = 2.0 * 3.14159265358979 * k / static_cast<double>(n);
    const double c = 2.0 * std::cos(w);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    for (const float x : mono) {
        s0 = x + (c * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    return static_cast<float>(std::sqrt(std::max(0.0, (s1 * s1) + (s2 * s2) - (c * s1 * s2))) /
                              (static_cast<double>(n) / 2.0));
}

} // namespace

TEST_CASE("engine equals direct FIR across impulse lengths", "[convolution]") {
    // Below/at/above one partition, a prime, and mid-tail — partition
    // padding must never leak into the result.
    const std::vector<std::size_t> lengths = {1, 127, 128, 129, 251, 1000};
    const std::vector<float> input = noise(0xC0FFEE, 4096);
    for (const std::size_t taps : lengths) {
        const std::vector<float> impulse =
            noise_impulse(0xBEEF + static_cast<std::uint32_t>(taps), taps);
        nt::audio::ConvolutionEngine engine;
        REQUIRE(engine.prepare(impulse, kBlock));
        REQUIRE(engine.ready());
        CHECK(engine.impulse_frames() == taps);

        const std::vector<float> got = run_engine(engine, input, {kBlock});
        const std::vector<float> want = direct_fir(impulse, input);
        float worst = 0.0F;
        for (std::size_t i = 0; i < want.size(); ++i) {
            worst = std::max(worst, std::abs(got[i] - want[i]));
        }
        INFO("taps=" << taps << " worst=" << worst);
        CHECK(worst < 1e-5F);
    }
}

TEST_CASE("engine output is call-phase invariant with zero latency", "[convolution]") {
    // The same stream cut into full blocks, single frames, and ragged
    // chunks must produce the same convolution — the sub-block path
    // re-applies the head partition, so no call pattern adds latency.
    const std::vector<float> impulse = noise_impulse(0xACE, 300);
    const std::vector<float> input = noise(0xF00D, 2048);
    const std::vector<float> want = direct_fir(impulse, input);

    const std::vector<std::vector<std::uint32_t>> patterns = {
        {kBlock}, {1}, {7}, {32}, {5, 3, 120, 128, 100, 28, 64},
    };
    for (const auto& pattern : patterns) {
        nt::audio::ConvolutionEngine engine;
        REQUIRE(engine.prepare(impulse, kBlock));
        const std::vector<float> got = run_engine(engine, input, pattern);
        float worst = 0.0F;
        for (std::size_t i = 0; i < want.size(); ++i) {
            worst = std::max(worst, std::abs(got[i] - want[i]));
        }
        INFO("pattern[0]=" << pattern[0] << " worst=" << worst);
        CHECK(worst < 1e-5F);
    }
}

TEST_CASE("a click through a 1 s impulse reproduces the impulse", "[convolution]") {
    // Exponentially decaying 440 Hz over one second: convolving a unit
    // click must play the impulse back verbatim — the long-tail case
    // the old 2048-tap FIR could never reach.
    constexpr std::uint32_t kRate = 48000;
    std::vector<float> impulse(kRate);
    for (std::uint32_t i = 0; i < kRate; ++i) {
        const float t = static_cast<float>(i) / kRate;
        impulse[i] = std::exp(-3.0F * t) * std::sin(2.0F * 3.14159265F * 440.0F * t);
    }
    nt::audio::ConvolutionEngine engine;
    REQUIRE(engine.prepare(impulse, kBlock));

    std::vector<float> input(kRate, 0.0F);
    input[0] = 1.0F;
    const std::vector<float> got = run_engine(engine, input, {kBlock});

    // Sample check: the response IS the impulse.
    float worst = 0.0F;
    for (std::uint32_t i = 0; i < kRate; ++i) {
        worst = std::max(worst, std::abs(got[i] - impulse[i]));
    }
    CHECK(worst < 1e-5F);

    // Spectral check: the 440 Hz line dominates deep into the tail.
    const std::vector<float> tail(got.begin() + (kRate / 2), got.begin() + (3 * kRate / 4));
    CHECK(goertzel(tail, kRate, 440.0F) > goertzel(tail, kRate, 620.0F) * 10.0F);
}

TEST_CASE("engine is deterministic run to run", "[convolution]") {
    const std::vector<float> impulse = noise(0xDEAD, 5000);
    const std::vector<float> input = noise(0xFACE, 4096);

    auto render = [&] {
        nt::audio::ConvolutionEngine engine;
        REQUIRE(engine.prepare(impulse, kBlock));
        return run_engine(engine, input, {kBlock});
    };
    const std::vector<float> first = render();
    const std::vector<float> second = render();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i] == second[i]); // bit-identical, not merely close
    }

    // reset() restores the initial history state exactly.
    nt::audio::ConvolutionEngine engine;
    REQUIRE(engine.prepare(impulse, kBlock));
    run_engine(engine, input, {kBlock});
    engine.reset();
    const std::vector<float> after_reset = run_engine(engine, input, {kBlock});
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(after_reset[i] == first[i]);
    }
}

TEST_CASE("process and reset run under the RT allocation guard", "[convolution][rt]") {
    // Debug builds abort on any audio-thread allocation inside an
    // RtScope (rt/rt_assert.h) — prepare() owns every buffer, so the
    // hot path must survive a 1 s impulse with ragged call sizes.
    constexpr std::uint32_t kRate = 48000;
    const std::vector<float> impulse = noise(0x5EED, kRate);
    nt::audio::ConvolutionEngine engine;
    REQUIRE(engine.prepare(impulse, kBlock));

    const std::vector<float> input = noise(0xAB1E, kBlock * 64);
    std::vector<float> out(input.size(), 0.0F);
    {
        [[maybe_unused]] const nt::rt::RtScope rt_scope;
        std::size_t at = 0;
        const std::uint32_t chunks[] = {kBlock, 32, 96, 1, 127};
        std::size_t chunk_index = 0;
        while (at < input.size()) {
            const auto n = static_cast<std::uint32_t>(
                std::min<std::size_t>(chunks[chunk_index % 5], input.size() - at));
            engine.process(input.data() + at, out.data() + at, n);
            at += n;
            ++chunk_index;
        }
        engine.reset();
        engine.process(input.data(), out.data(), kBlock);
    }
    float peak = 0.0F;
    for (const float v : out) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak > 0.0F); // the guard test still has to do real work
}
