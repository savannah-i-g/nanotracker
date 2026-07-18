// Spectral-flux onset detector verification (Stage 27): synthetic
// fixtures with transients at known sample offsets, level-invariance,
// the no-transient cases (steady tone, DC, silence) that must report
// nothing, the strongest-N cap, and determinism. The detector is pure —
// these tests exercise it directly, with no plugin or archive around it.
#include "audio/onset_detect.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<float> silence(float seconds, std::uint32_t rate = kRate) {
    return std::vector<float>(static_cast<std::size_t>(seconds * static_cast<float>(rate)), 0.0F);
}

// A percussive hit: sharp attack, exponential decay to silence (no
// abrupt cutoff, so exactly one transient per hit).
void add_pluck(std::vector<float>& x, std::size_t at, float freq, float amp, std::uint32_t rate) {
    const auto len = static_cast<std::size_t>(0.15F * static_cast<float>(rate));
    for (std::size_t i = 0; i < len && at + i < x.size(); ++i) {
        const float env = std::exp(-static_cast<float>(i) / (0.03F * static_cast<float>(rate)));
        x[at + i] += amp * env *
                     std::sin(2.0F * std::numbers::pi_v<float> * freq * static_cast<float>(i) /
                              static_cast<float>(rate));
    }
}

// True when every known offset has a detected onset within `tol`.
bool all_matched(const std::vector<std::uint32_t>& got, const std::vector<std::size_t>& known,
                 std::uint32_t tol) {
    for (const std::size_t k : known) {
        bool hit = false;
        for (const std::uint32_t g : got) {
            const auto d = g > k ? g - k : static_cast<std::uint32_t>(k) - g;
            hit = hit || d <= tol;
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("onset detector finds transients at known offsets within a hop", "[onset]") {
    const auto hop = nt::audio::onset_analysis_for(kRate).hop;
    const std::vector<std::size_t> known = {4800, 14400, 24000, 33600};
    const std::vector<float> freqs = {380.0F, 620.0F, 860.0F, 1100.0F};

    std::vector<float> x = silence(1.0F);
    for (std::size_t i = 0; i < known.size(); ++i) {
        add_pluck(x, known[i], freqs[i], 0.7F, kRate);
    }
    const auto onsets = nt::audio::detect_onsets(x.data(), x.size(), kRate, 92);

    CHECK(onsets.size() == known.size());
    CHECK(all_matched(onsets, known, hop)); // every hit within one hop
}

TEST_CASE("onset detection is level-invariant", "[onset]") {
    const std::vector<std::size_t> known = {6000, 18000, 30000};
    std::vector<float> loud = silence(0.9F);
    for (const std::size_t k : known) {
        add_pluck(loud, k, 500.0F, 0.8F, kRate);
    }
    std::vector<float> quiet = loud;
    for (float& v : quiet) {
        v *= 0.5F; // −6 dB
    }
    const auto a = nt::audio::detect_onsets(loud.data(), loud.size(), kRate, 92);
    const auto b = nt::audio::detect_onsets(quiet.data(), quiet.size(), kRate, 92);
    CHECK(a == b); // same offsets at half amplitude
    CHECK(a.size() == known.size());
}

TEST_CASE("steady content yields no transients", "[onset]") {
    SECTION("a pure sine filling the buffer") {
        std::vector<float> x = silence(1.0F);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] = 0.7F * std::sin(2.0F * std::numbers::pi_v<float> * 440.0F *
                                   static_cast<float>(i) / static_cast<float>(kRate));
        }
        // A sustained tone has near-zero flux; 0 or 1 onset is acceptable.
        CHECK(nt::audio::detect_onsets(x.data(), x.size(), kRate, 92).size() <= 1);
    }
    SECTION("a DC offset") {
        std::vector<float> x(kRate, 0.5F);
        CHECK(nt::audio::detect_onsets(x.data(), x.size(), kRate, 92).empty());
    }
    SECTION("digital silence") {
        std::vector<float> x = silence(1.0F);
        CHECK(nt::audio::detect_onsets(x.data(), x.size(), kRate, 92).empty());
    }
    SECTION("a buffer shorter than one analysis window") {
        std::vector<float> x(64, 0.9F);
        CHECK(nt::audio::detect_onsets(x.data(), x.size(), kRate, 92).empty());
    }
}

TEST_CASE("onset count is capped to the strongest N", "[onset]") {
    std::vector<float> x = silence(3.0F);
    for (int k = 0; k < 30; ++k) { // 80 ms apart, above the min-gap
        add_pluck(x, 2000 + (static_cast<std::size_t>(k) * 3840), 700.0F, 0.8F, kRate);
    }
    const auto full = nt::audio::detect_onsets(x.data(), x.size(), kRate, 92);
    const auto capped = nt::audio::detect_onsets(x.data(), x.size(), kRate, 8);
    CHECK(full.size() > 8);    // more transients than the cap
    CHECK(capped.size() == 8); // exactly the cap survives
    // Determinism: identical input, identical output.
    CHECK(capped == nt::audio::detect_onsets(x.data(), x.size(), kRate, 8));
}

TEST_CASE("onset detection works across sample rates", "[onset]") {
    for (const std::uint32_t rate : {44100U, 96000U}) {
        const auto hop = nt::audio::onset_analysis_for(rate).hop;
        const std::vector<std::size_t> known = {
            static_cast<std::size_t>(0.1F * static_cast<float>(rate)),
            static_cast<std::size_t>(0.4F * static_cast<float>(rate)),
            static_cast<std::size_t>(0.7F * static_cast<float>(rate))};
        std::vector<float> x = silence(1.0F, rate);
        for (const std::size_t k : known) {
            add_pluck(x, k, 550.0F, 0.8F, rate);
        }
        const auto onsets = nt::audio::detect_onsets(x.data(), x.size(), rate, 92);
        CHECK(onsets.size() == known.size());
        CHECK(all_matched(onsets, known, hop));
    }
}
