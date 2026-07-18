// FX chain DSP verification: deterministic block processing, no audio
// device. Each module proves its defining behaviour.
#include "audio/fx_chain.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

nt::engine::FxModuleInstance instance_of(const char* id,
                                         std::vector<std::pair<std::string, float>> params) {
    nt::engine::FxModuleInstance instance;
    instance.module_id = id;
    instance.params = std::move(params);
    return instance;
}

} // namespace

TEST_CASE("delay module echoes at the configured time", "[fx]") {
    const auto* def = nt::audio::fx_module_by_id("delay");
    REQUIRE(def != nullptr);
    // 100% wet, no dry, 0.1s, no feedback @ 1000 Hz rate → echo at
    // frame 100.
    nt::audio::FxModuleRuntime delay(
        *def, instance_of("delay", {{"wet", 100}, {"dry", 0}, {"time", 0.1F}, {"feedback", 0}}),
        1000);

    std::vector<float> left(256, 0.0F);
    std::vector<float> right(256, 0.0F);
    left[0] = 1.0F;
    right[0] = 1.0F;
    delay.process(left.data(), right.data(), 256);

    // The impulse must vanish from t=0 (no dry) and appear near 100.
    CHECK(std::abs(left[0]) < 0.01F);
    float peak = 0.0F;
    std::size_t peak_at = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::abs(left[i]) > peak) {
            peak = std::abs(left[i]);
            peak_at = i;
        }
    }
    CHECK(peak > 0.5F);
    CHECK(peak_at >= 98);
    CHECK(peak_at <= 102);
}

TEST_CASE("bitcrusher quantises to the configured depth", "[fx]") {
    const auto* def = nt::audio::fx_module_by_id("bitcrusher");
    REQUIRE(def != nullptr);
    // 2 bits, hold rate = host rate (every sample), fully wet.
    nt::audio::FxModuleRuntime crusher(
        *def,
        instance_of("bitcrusher", {{"bits", 2}, {"sampleRate", 48000}, {"wet", 100}, {"dry", 0}}),
        48000);

    std::vector<float> left(64);
    std::vector<float> right(64);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = right[i] = static_cast<float>(i) / 64.0F;
    }
    crusher.process(left.data(), right.data(), 64);

    // 2-bit quantisation leaves only multiples of 0.25.
    for (const float v : left) {
        const float nearest = std::round(v * 4.0F) / 4.0F;
        CHECK(std::abs(v - nearest) < 1e-4F);
    }
}

TEST_CASE("stereo width at zero collapses to mono", "[fx]") {
    const auto* def = nt::audio::fx_module_by_id("stereo-width");
    REQUIRE(def != nullptr);
    nt::audio::FxModuleRuntime width(*def, instance_of("stereo-width", {{"width", 0}, {"pan", 0}}),
                                     48000);

    std::vector<float> left(32, 1.0F);
    std::vector<float> right(32, -1.0F);
    width.process(left.data(), right.data(), 32);
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK(std::abs(left[i] - right[i]) < 1e-5F); // pure mid
        CHECK(std::abs(left[i]) < 1e-5F);            // mid of (1,-1) = 0
    }
}

TEST_CASE("rack routes sends and applies automation", "[fx]") {
    nt::engine::FxMixerState state;
    nt::engine::FxChannelStrip strip;
    strip.name = "FX 01";
    strip.enabled = true;
    strip.tracker_sends = {1.0F, 0.0F};
    strip.modules.push_back(
        instance_of("delay", {{"wet", 100}, {"dry", 0}, {"time", 0.05F}, {"feedback", 0}}));
    state.channels.push_back(strip);

    nt::audio::FxRack rack(state, 1000);

    std::vector<float> ch0(nt::audio::kFxBlockFrames * 2, 0.0F);
    std::vector<float> ch1(nt::audio::kFxBlockFrames * 2, 0.0F);
    ch0[0] = ch0[1] = 1.0F; // impulse only on tracker channel 0
    ch1[0] = ch1[1] = 0.5F; // must NOT reach the bus (send = 0)
    const float* channels[2] = {ch0.data(), ch1.data()};

    std::vector<float> out(nt::audio::kFxBlockFrames * 2, 0.0F);
    rack.process(channels, 2, out.data(), nt::audio::kFxBlockFrames);

    // Echo of channel 0's unit impulse at ~frame 50, nothing at 0.
    CHECK(std::abs(out[0]) < 0.01F);
    float peak = 0.0F;
    std::size_t peak_at = 0;
    for (std::size_t i = 0; i < nt::audio::kFxBlockFrames; ++i) {
        if (std::abs(out[i * 2]) > peak) {
            peak = std::abs(out[i * 2]);
            peak_at = i;
        }
    }
    CHECK(peak > 0.4F);
    CHECK(peak_at >= 48);
    CHECK(peak_at <= 52);

    // Automation flips the module fully dry → silence next block.
    nt::engine::FxCell cell;
    cell.channel_index = 0;
    cell.module_index = 0;
    cell.param_key = "wet";
    cell.value = 0;
    rack.apply_automation(cell);
    nt::engine::FxCell dry_cell = cell;
    dry_cell.param_key = "dry";
    dry_cell.value = 0;
    rack.apply_automation(dry_cell);

    std::fill(out.begin(), out.end(), 0.0F);
    std::fill(ch0.begin(), ch0.end(), 0.0F);
    rack.process(channels, 2, out.data(), nt::audio::kFxBlockFrames);
    float energy = 0.0F;
    for (const float v : out) {
        energy += v * v;
    }
    CHECK(energy < 1e-4F);
}
