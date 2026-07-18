// audio/fx_chain — the native FX mixer: eight built-in modules with
// the web modules' exact parameter schemas and signal topologies
// (Source/.../src/components/trackerFxModules/*), assembled into
// channel strips fed by per-tracker-channel sends.
//
// Documented divergence: REVERB is a Freeverb-style algorithmic
// reverb with the web module's parameters (wet/dry/decay/preDelay)
// instead of convolution with a synthetic impulse — WebAudio provided
// the convolver for free; natively the algorithmic form is the right
// cost. Recorded in Docs/FIXES.md.
//
// Threading: an FxRack is built OFF the audio thread (allocations) and
// handed over with the playback bundle; process() and set_param() run
// on the audio thread and never allocate.
#pragma once

#include "audio/dsp.h"
#include "engine/tracker_types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nt::audio {

inline constexpr std::uint32_t kFxBlockFrames = 128;

struct FxParamDef {
    const char* key;
    const char* label;
    float min;
    float max;
    float def;
    float step;
};

struct FxModuleDef {
    const char* id;
    const char* name;
    std::span<const FxParamDef> params;
};

// The eight built-in modules, in registry order.
std::span<const FxModuleDef> fx_module_registry();
const FxModuleDef* fx_module_by_id(const std::string& id);

// One live module instance (type-erased over the eight kinds).
class FxModuleRuntime {
public:
    FxModuleRuntime(const FxModuleDef& def, const engine::FxModuleInstance& instance,
                    std::uint32_t rate);

    void process(float* left, float* right, std::uint32_t frames);
    bool set_param(const std::string& key, float value); // audio thread, no alloc
    bool set_param_index(std::size_t index, float value);

    [[nodiscard]] const FxModuleDef& def() const { return *def_; }

    [[nodiscard]] bool enabled() const { return enabled_; }

    void set_enabled(bool enabled) { enabled_ = enabled; }

    [[nodiscard]] float param(std::size_t index) const { return params_[index]; }

private:
    void refresh(); // re-derive DSP state from params_

    const FxModuleDef* def_;
    std::uint32_t rate_;
    bool enabled_ = true;
    std::array<float, 8> params_{};

    // Union-of-needs DSP state; only the members the module kind uses
    // are touched. Sized for the largest (reverb).
    dsp::DelayLine delay_l_;
    dsp::DelayLine delay_r_;
    dsp::Biquad biquad_l_;
    dsp::Biquad biquad_r_;
    dsp::Bitcrush crush_l_;
    dsp::Bitcrush crush_r_;
    dsp::Compressor compressor_;
    double lfo_phase_ = 0.0;
    std::array<dsp::Comb, 8> combs_l_;
    std::array<dsp::Comb, 8> combs_r_;
    std::array<dsp::Allpass, 4> aps_l_;
    std::array<dsp::Allpass, 4> aps_r_;
};

// A built strip: module chain + volume/pan + sends.
class FxChannelRuntime {
public:
    FxChannelRuntime(const engine::FxChannelStrip& strip, std::uint32_t rate);

    // Mixes `frames` from `bus` (this strip's send sum) through the
    // chain and adds into `out_l/out_r`.
    void process(const float* bus_l, const float* bus_r, float* out_l, float* out_r,
                 std::uint32_t frames);

    [[nodiscard]] bool enabled() const { return enabled_; }

    [[nodiscard]] float send(int tracker_channel) const;
    bool set_param(int module_index, const std::string& key, float value);

    std::vector<std::unique_ptr<FxModuleRuntime>>& modules() { return modules_; }

private:
    bool enabled_ = true;
    float volume_ = 1.0F; // 0-100 → 0-1
    float pan_ = 0.0F;    // -1..1
    std::vector<float> sends_;
    std::vector<std::unique_ptr<FxModuleRuntime>> modules_;
    std::array<float, kFxBlockFrames> scratch_l_{};
    std::array<float, kFxBlockFrames> scratch_r_{};
};

// The whole mixer: strips built from the project's FX state.
class FxRack {
public:
    FxRack(const engine::FxMixerState& state, std::uint32_t rate);

    // channel_scratch: per-tracker-channel interleaved stereo blocks.
    void process(const float* const* channel_scratch, int tracker_channels, float* out_interleaved,
                 std::uint32_t frames);

    // Audio thread: automation/param writes (no allocation).
    void apply_automation(const engine::FxCell& cell);
    bool set_param(int channel, int module_index, const std::string& key, float value);
    bool set_param_index(int channel, int module_index, int param_index, float value);

private:
    std::vector<std::unique_ptr<FxChannelRuntime>> channels_;
    std::array<float, kFxBlockFrames> bus_l_{};
    std::array<float, kFxBlockFrames> bus_r_{};
};

} // namespace nt::audio
