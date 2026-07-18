#include "audio/fx_chain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nt::audio {

namespace {

// Parameter schemas mirror the web module definitions exactly.
constexpr std::array<FxParamDef, 4> kDelayParams = {{
    {"wet", "WET", 0, 100, 30, 1},
    {"dry", "DRY", 0, 100, 100, 1},
    {"time", "TIME", 0.01F, 2.0F, 0.375F, 0.001F},
    {"feedback", "FEEDBACK", 0, 95, 35, 1},
}};
constexpr std::array<FxParamDef, 4> kFilterParams = {{
    {"type", "TYPE", 0, 2, 0, 1},
    {"freq", "FREQ", 20, 20000, 1000, 1},
    {"q", "Q", 0.1F, 30, 1.0F, 0.1F},
    {"gain", "GAIN", -24, 24, 0, 0.5F},
}};
constexpr std::array<FxParamDef, 4> kBitcrusherParams = {{
    {"bits", "BITS", 2, 16, 8, 1},
    {"sampleRate", "RATE", 1000, 44100, 22050, 100},
    {"wet", "WET", 0, 100, 100, 1},
    {"dry", "DRY", 0, 100, 0, 1},
}};
constexpr std::array<FxParamDef, 6> kCompressorParams = {{
    {"threshold", "THRESHOLD", -60, 0, -18, 1},
    {"ratio", "RATIO", 1, 20, 4, 0.5F},
    {"attack", "ATTACK", 0.001F, 1, 0.003F, 0.001F},
    {"release", "RELEASE", 0.01F, 2, 0.25F, 0.01F},
    {"knee", "KNEE", 0, 40, 3, 1},
    {"makeupGain", "MAKEUP", 0, 24, 0, 0.5F},
}};
constexpr std::array<FxParamDef, 4> kDistortionParams = {{
    {"drive", "DRIVE", 0, 100, 30, 1},
    {"tone", "TONE", 200, 8000, 3000, 10},
    {"wet", "WET", 0, 100, 100, 1},
    {"dry", "DRY", 0, 100, 0, 1},
}};
constexpr std::array<FxParamDef, 7> kChorusParams = {{
    {"rate", "RATE", 0.1F, 10, 1.0F, 0.1F},
    {"depth", "DEPTH", 0, 25, 5.0F, 0.5F},
    {"delay", "DELAY", 1, 30, 10, 0.5F},
    {"feedback", "FEEDBACK", 0, 90, 0, 1},
    {"wet", "WET", 0, 100, 50, 1},
    {"dry", "DRY", 0, 100, 100, 1},
    {"mode", "MODE", 0, 1, 0, 1},
}};
constexpr std::array<FxParamDef, 4> kReverbParams = {{
    {"wet", "WET", 0, 100, 30, 1},
    {"dry", "DRY", 0, 100, 100, 1},
    {"decay", "DECAY", 0.1F, 8, 2.0F, 0.1F},
    {"preDelay", "PRE-DELAY", 0, 100, 0, 1},
}};
constexpr std::array<FxParamDef, 2> kWidthParams = {{
    {"width", "WIDTH", 0, 200, 100, 1},
    {"pan", "PAN", -100, 100, 0, 1},
}};

constexpr std::array<FxModuleDef, 8> kRegistry = {{
    {"delay", "DELAY", kDelayParams},
    {"filter", "FILTER", kFilterParams},
    {"bitcrusher", "BITCRUSHER", kBitcrusherParams},
    {"compressor", "COMPRESSOR", kCompressorParams},
    {"distortion", "DISTORTION", kDistortionParams},
    {"chorus-flanger", "CHORUS / FLGR", kChorusParams},
    {"reverb", "REVERB", kReverbParams},
    {"stereo-width", "STEREO WIDTH", kWidthParams},
}};

enum class Kind : std::uint8_t {
    kDelay,
    kFilter,
    kBitcrusher,
    kCompressor,
    kDistortion,
    kChorus,
    kReverb,
    kWidth,
};

Kind kind_of(const FxModuleDef& def) {
    for (std::size_t i = 0; i < kRegistry.size(); ++i) {
        if (&kRegistry[i] == &def) {
            return static_cast<Kind>(i);
        }
    }
    return Kind::kDelay;
}

// The web Distortion's sigmoid: y = (π+k)x / (π+k|x|), k = drive/100*400.
float sigmoid_shape(float x, float drive) {
    const float k = std::max(0.01F, drive / 100.0F) * 400.0F;
    constexpr float kPi = std::numbers::pi_v<float>;
    return ((kPi + k) * x) / (kPi + (k * std::abs(x)));
}

// Freeverb tunings (44.1k reference, scaled to rate at configure).
constexpr std::array<int, 8> kCombTunings = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<int, 4> kAllpassTunings = {556, 441, 341, 225};

} // namespace

std::span<const FxModuleDef> fx_module_registry() {
    return kRegistry;
}

const FxModuleDef* fx_module_by_id(const std::string& id) {
    for (const FxModuleDef& def : kRegistry) {
        if (id == def.id) {
            return &def;
        }
    }
    return nullptr;
}

FxModuleRuntime::FxModuleRuntime(const FxModuleDef& def, const engine::FxModuleInstance& instance,
                                 std::uint32_t rate)
    : def_(&def), rate_(rate), enabled_(instance.enabled) {
    for (std::size_t i = 0; i < def.params.size(); ++i) {
        params_[i] = def.params[i].def;
        for (const auto& [key, value] : instance.params) {
            if (key == def.params[i].key) {
                params_[i] = value;
            }
        }
    }

    const Kind kind = kind_of(def);
    if (kind == Kind::kDelay) {
        delay_l_.configure(rate * 2 + 4);
        delay_r_.configure(rate * 2 + 4);
    } else if (kind == Kind::kChorus) {
        delay_l_.configure(rate / 8);
        delay_r_.configure(rate / 8);
    } else if (kind == Kind::kReverb) {
        const double scale = static_cast<double>(rate) / 44100.0;
        for (std::size_t i = 0; i < combs_l_.size(); ++i) {
            combs_l_[i].configure(static_cast<std::uint32_t>(kCombTunings[i] * scale));
            combs_r_[i].configure(static_cast<std::uint32_t>((kCombTunings[i] + 23) * scale));
        }
        for (std::size_t i = 0; i < aps_l_.size(); ++i) {
            aps_l_[i].configure(static_cast<std::uint32_t>(kAllpassTunings[i] * scale));
            aps_r_[i].configure(static_cast<std::uint32_t>((kAllpassTunings[i] + 23) * scale));
        }
        delay_l_.configure(rate / 8); // pre-delay
        delay_r_.configure(rate / 8);
    }
    crush_l_.configure(rate);
    crush_r_.configure(rate);
    compressor_.configure(rate);
    refresh();
}

void FxModuleRuntime::refresh() {
    switch (kind_of(*def_)) {
    case Kind::kFilter: {
        const auto type = static_cast<dsp::Biquad::Type>(
            std::clamp(static_cast<int>(std::round(params_[0])), 0, 2));
        biquad_l_.configure(type, params_[1], params_[2], rate_);
        biquad_r_.configure(type, params_[1], params_[2], rate_);
        break;
    }
    case Kind::kBitcrusher:
        crush_l_.set(params_[0], params_[1]);
        crush_r_.set(params_[0], params_[1]);
        break;
    case Kind::kCompressor:
        compressor_.set(params_[0], params_[1], params_[2], params_[3], params_[4]);
        break;
    case Kind::kDistortion:
        biquad_l_.configure(dsp::Biquad::Type::kLowpass, params_[1], 0.707F, rate_);
        biquad_r_.configure(dsp::Biquad::Type::kLowpass, params_[1], 0.707F, rate_);
        break;
    case Kind::kReverb: {
        // Map decay seconds → comb feedback (Freeverb roomsize feel).
        const float decay = std::clamp(params_[2], 0.1F, 8.0F);
        const float feedback = std::clamp(0.7F + (decay / 8.0F) * 0.28F, 0.7F, 0.98F);
        for (auto& c : combs_l_) {
            c.set(feedback, 0.2F);
        }
        for (auto& c : combs_r_) {
            c.set(feedback, 0.2F);
        }
        break;
    }
    default:
        break;
    }
}

bool FxModuleRuntime::set_param_index(std::size_t index, float value) {
    if (index >= def_->params.size()) {
        return false;
    }
    params_[index] = std::clamp(value, def_->params[index].min, def_->params[index].max);
    refresh();
    return true;
}

bool FxModuleRuntime::set_param(const std::string& key, float value) {
    for (std::size_t i = 0; i < def_->params.size(); ++i) {
        if (key == def_->params[i].key) {
            params_[i] = std::clamp(value, def_->params[i].min, def_->params[i].max);
            refresh();
            return true;
        }
    }
    return false;
}

void FxModuleRuntime::process(float* left, float* right, std::uint32_t frames) {
    if (!enabled_) {
        return;
    }
    switch (kind_of(*def_)) {
    case Kind::kDelay: {
        const float wet = params_[0] / 100.0F;
        const float dry = params_[1] / 100.0F;
        const float time_frames = params_[2] * static_cast<float>(rate_);
        const float feedback = params_[3] / 100.0F;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float dl = delay_l_.read(time_frames);
            const float dr = delay_r_.read(time_frames);
            delay_l_.write(left[i] + (dl * feedback));
            delay_r_.write(right[i] + (dr * feedback));
            left[i] = (left[i] * dry) + (dl * wet);
            right[i] = (right[i] * dry) + (dr * wet);
        }
        break;
    }
    case Kind::kFilter:
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] = biquad_l_.process(left[i]);
            right[i] = biquad_r_.process(right[i]);
        }
        break;
    case Kind::kBitcrusher: {
        const float wet = params_[2] / 100.0F;
        const float dry = params_[3] / 100.0F;
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] = (left[i] * dry) + (crush_l_.process(left[i]) * wet);
            right[i] = (right[i] * dry) + (crush_r_.process(right[i]) * wet);
        }
        break;
    }
    case Kind::kCompressor: {
        const float makeup = std::pow(10.0F, params_[5] / 20.0F);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float g = compressor_.gain_for(left[i], right[i]) * makeup;
            left[i] *= g;
            right[i] *= g;
        }
        break;
    }
    case Kind::kDistortion: {
        const float drive = params_[0];
        const float wet = params_[2] / 100.0F;
        const float dry = params_[3] / 100.0F;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float sl = biquad_l_.process(sigmoid_shape(left[i], drive));
            const float sr = biquad_r_.process(sigmoid_shape(right[i], drive));
            left[i] = (left[i] * dry) + (sl * wet);
            right[i] = (right[i] * dry) + (sr * wet);
        }
        break;
    }
    case Kind::kChorus: {
        const float rate_hz = params_[0];
        const float depth_frames = (params_[1] / 1000.0F) * static_cast<float>(rate_);
        const float base_frames = (params_[2] / 1000.0F) * static_cast<float>(rate_);
        const float feedback = params_[3] / 100.0F;
        const float wet = params_[4] / 100.0F;
        const float dry = params_[5] / 100.0F;
        const double step = 2.0 * std::numbers::pi * rate_hz / static_cast<double>(rate_);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto mod = static_cast<float>(std::sin(lfo_phase_));
            lfo_phase_ += step;
            const float d = std::max(1.0F, base_frames + (mod * depth_frames));
            const float dl = delay_l_.read(d);
            const float dr = delay_r_.read(d);
            delay_l_.write(left[i] + (dl * feedback));
            delay_r_.write(right[i] + (dr * feedback));
            left[i] = (left[i] * dry) + (dl * wet);
            right[i] = (right[i] * dry) + (dr * wet);
        }
        if (lfo_phase_ > 2.0 * std::numbers::pi) {
            lfo_phase_ = std::fmod(lfo_phase_, 2.0 * std::numbers::pi);
        }
        break;
    }
    case Kind::kReverb: {
        const float wet = (params_[0] / 100.0F) * 0.3F; // Freeverb wet scale
        const float dry = params_[1] / 100.0F;
        const float predelay_frames = (params_[3] / 1000.0F) * static_cast<float>(rate_);
        for (std::uint32_t i = 0; i < frames; ++i) {
            delay_l_.write(left[i]);
            delay_r_.write(right[i]);
            const float in_l = predelay_frames > 1 ? delay_l_.read(predelay_frames) : left[i];
            const float in_r = predelay_frames > 1 ? delay_r_.read(predelay_frames) : right[i];
            const float mixed = (in_l + in_r) * 0.5F;
            float rl = 0.0F;
            float rr = 0.0F;
            for (auto& c : combs_l_) {
                rl += c.process(mixed);
            }
            for (auto& c : combs_r_) {
                rr += c.process(mixed);
            }
            for (auto& a : aps_l_) {
                rl = a.process(rl);
            }
            for (auto& a : aps_r_) {
                rr = a.process(rr);
            }
            left[i] = (left[i] * dry) + (rl * wet);
            right[i] = (right[i] * dry) + (rr * wet);
        }
        break;
    }
    case Kind::kWidth: {
        const float width = params_[0] / 100.0F;
        const float pan = params_[1] / 100.0F;
        const float pan_l = std::min(1.0F, 1.0F - pan);
        const float pan_r = std::min(1.0F, 1.0F + pan);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float mid = (left[i] + right[i]) * 0.5F;
            const float side = (left[i] - right[i]) * 0.5F * width;
            left[i] = (mid + side) * pan_l;
            right[i] = (mid - side) * pan_r;
        }
        break;
    }
    }
}

FxChannelRuntime::FxChannelRuntime(const engine::FxChannelStrip& strip, std::uint32_t rate)
    : enabled_(strip.enabled), volume_(static_cast<float>(strip.volume) / 100.0F),
      pan_(static_cast<float>(strip.pan) / 100.0F), sends_(strip.tracker_sends) {
    for (const engine::FxModuleInstance& instance : strip.modules) {
        if (const FxModuleDef* def = fx_module_by_id(instance.module_id); def != nullptr) {
            modules_.push_back(std::make_unique<FxModuleRuntime>(*def, instance, rate));
        }
    }
}

float FxChannelRuntime::send(int tracker_channel) const {
    if (tracker_channel < 0 || tracker_channel >= static_cast<int>(sends_.size())) {
        return 0.0F;
    }
    return sends_[static_cast<std::size_t>(tracker_channel)];
}

bool FxChannelRuntime::set_param(int module_index, const std::string& key, float value) {
    if (module_index < 0 || module_index >= static_cast<int>(modules_.size())) {
        return false;
    }
    return modules_[static_cast<std::size_t>(module_index)]->set_param(key, value);
}

void FxChannelRuntime::process(const float* bus_l, const float* bus_r, float* out_l, float* out_r,
                               std::uint32_t frames) {
    if (!enabled_) {
        return;
    }
    std::copy_n(bus_l, frames, scratch_l_.data());
    std::copy_n(bus_r, frames, scratch_r_.data());
    for (auto& module : modules_) {
        module->process(scratch_l_.data(), scratch_r_.data(), frames);
    }
    const float gain_l = volume_ * std::min(1.0F, 1.0F - pan_);
    const float gain_r = volume_ * std::min(1.0F, 1.0F + pan_);
    for (std::uint32_t i = 0; i < frames; ++i) {
        out_l[i] += scratch_l_[i] * gain_l;
        out_r[i] += scratch_r_[i] * gain_r;
    }
}

FxRack::FxRack(const engine::FxMixerState& state, std::uint32_t rate) {
    for (const engine::FxChannelStrip& strip : state.channels) {
        channels_.push_back(std::make_unique<FxChannelRuntime>(strip, rate));
    }
}

void FxRack::process(const float* const* channel_scratch, int tracker_channels,
                     float* out_interleaved, std::uint32_t frames) {
    if (channels_.empty()) {
        return;
    }
    std::array<float, kFxBlockFrames> out_l{};
    std::array<float, kFxBlockFrames> out_r{};

    for (auto& channel : channels_) {
        bus_l_.fill(0.0F);
        bus_r_.fill(0.0F);
        bool any = false;
        for (int ch = 0; ch < tracker_channels; ++ch) {
            const float amount = channel->send(ch);
            if (amount <= 0.0F) {
                continue;
            }
            any = true;
            const float* scratch = channel_scratch[ch];
            for (std::uint32_t i = 0; i < frames; ++i) {
                const std::size_t at = static_cast<std::size_t>(i) * 2;
                bus_l_[i] += scratch[at] * amount;
                bus_r_[i] += scratch[at + 1] * amount;
            }
        }
        if (any || !channel->modules().empty()) {
            channel->process(bus_l_.data(), bus_r_.data(), out_l.data(), out_r.data(), frames);
        }
    }

    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::size_t at = static_cast<std::size_t>(i) * 2;
        out_interleaved[at] += out_l[i];
        out_interleaved[at + 1] += out_r[i];
    }
}

void FxRack::apply_automation(const engine::FxCell& cell) {
    set_param(cell.channel_index, cell.module_index, cell.param_key,
              static_cast<float>(cell.value));
}

bool FxRack::set_param(int channel, int module_index, const std::string& key, float value) {
    if (channel < 0 || channel >= static_cast<int>(channels_.size())) {
        return false;
    }
    return channels_[static_cast<std::size_t>(channel)]->set_param(module_index, key, value);
}

bool FxRack::set_param_index(int channel, int module_index, int param_index, float value) {
    if (channel < 0 || channel >= static_cast<int>(channels_.size())) {
        return false;
    }
    auto& modules = channels_[static_cast<std::size_t>(channel)]->modules();
    if (module_index < 0 || module_index >= static_cast<int>(modules.size())) {
        return false;
    }
    return modules[static_cast<std::size_t>(module_index)]->set_param_index(
        static_cast<std::size_t>(param_index), value);
}

} // namespace nt::audio
