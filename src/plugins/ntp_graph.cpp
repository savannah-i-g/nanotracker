#include "plugins/ntp_graph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace nt::plugins {

namespace {

// Deterministic xorshift for grain jitter and sample-and-hold — the
// audio thread never calls the library RNG.
inline std::uint32_t next_rng(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

inline float rng01(std::uint32_t& state) {
    return static_cast<float>(next_rng(state) & 0xFFFFFF) / static_cast<float>(0x1000000);
}

inline float downmix(const float* stereo, std::uint32_t frame) {
    return (stereo[static_cast<std::size_t>(frame) * 2] +
            stereo[(static_cast<std::size_t>(frame) * 2) + 1]) *
           0.5F;
}

// Interns a group name into `names`, returning its stable index (-1
// for "no group"). Zones and slices share this machinery; choke groups
// additionally share one namespace per node, so a slice can cut a
// zone's voice and vice versa when authors reuse a name.
int intern_group(std::vector<std::string>& names, const std::string& name) {
    if (name.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<int>(i);
        }
    }
    names.push_back(name);
    return static_cast<int>(names.size() - 1);
}

audio::dsp::Biquad::Type biquad_type_from(const std::string& name) {
    if (name == "highpass") {
        return audio::dsp::Biquad::Type::kHighpass;
    }
    if (name == "bandpass") {
        return audio::dsp::Biquad::Type::kBandpass;
    }
    if (name == "notch") {
        return audio::dsp::Biquad::Type::kNotch;
    }
    return audio::dsp::Biquad::Type::kLowpass;
}

} // namespace

NodeRuntime::NodeRuntime(const ntp::DspNode& def, const LoadedNtpPlugin& plugin, std::uint32_t rate,
                         std::uint32_t seed)
    : def_(&def), rate_(rate), rng_(seed | 1U) {
    auto add_param = [this](const char* name, float base) {
        params_[static_cast<std::size_t>(param_count_)] = {.name = name, .base = base};
        ++param_count_;
    };

    switch (def.type) {
    case ntp::NodeType::kGain:
    case ntp::NodeType::kMixer:
        add_param("gain", static_cast<float>(def.gain));
        break;
    case ntp::NodeType::kDelay:
        add_param("delayTime", static_cast<float>(def.delay_time));
        delay_l_.configure(static_cast<std::uint32_t>(
            std::max(1.0, def.max_delay * static_cast<double>(rate)) + 4));
        delay_r_.configure(static_cast<std::uint32_t>(
            std::max(1.0, def.max_delay * static_cast<double>(rate)) + 4));
        break;
    case ntp::NodeType::kBiquad:
        add_param("frequency", static_cast<float>(def.frequency));
        add_param("Q", static_cast<float>(def.q));
        break;
    case ntp::NodeType::kCompressor:
        add_param("threshold", static_cast<float>(def.threshold));
        add_param("ratio", static_cast<float>(def.ratio));
        add_param("attack", static_cast<float>(def.attack));
        add_param("release", static_cast<float>(def.release));
        add_param("knee", static_cast<float>(def.knee));
        compressor_.configure(rate);
        break;
    case ntp::NodeType::kPanner:
        add_param("pan", static_cast<float>(def.pan));
        break;
    case ntp::NodeType::kWaveshaper:
        add_param("drive", static_cast<float>(def.drive));
        break;
    case ntp::NodeType::kOscillator:
        add_param("frequency", static_cast<float>(def.frequency));
        add_param("detune", 0.0F);
        add_param("gain", static_cast<float>(def.gain));
        break;
    case ntp::NodeType::kNoise:
        add_param("gain", static_cast<float>(def.gain));
        break;
    case ntp::NodeType::kConstant:
        add_param("value", static_cast<float>(def.constant_value));
        break;
    case ntp::NodeType::kLfo:
        add_param("rate", static_cast<float>(def.lfo_rate));
        add_param("depth", static_cast<float>(def.lfo_depth));
        break;
    case ntp::NodeType::kEnvelope:
        add_param("release", static_cast<float>(def.release));
        break;
    case ntp::NodeType::kGranular: {
        add_param("position", 0.0F);
        add_param("density", 20.0F);
        add_param("grainSize", 0.08F);
        add_param("pitch", 0.0F);
        add_param("scanRate", 0.0F);
        add_param("pan", 0.0F);
        add_param("gate", 1.0F);
        add_param("positionJitter", 0.0F);
        add_param("pitchJitter", 0.0F);
        add_param("sizeJitter", 0.0F);
        add_param("panJitter", 0.0F);
        grains_.resize(kMaxGrains);
        const auto it = plugin.samples.find(def.sample_file);
        source_buffer_ = it != plugin.samples.end() ? it->second.get() : nullptr;
        break;
    }
    case ntp::NodeType::kWavetable: {
        add_param("frequency", 440.0F);
        add_param("framePosition", 0.0F);
        add_param("detune", 0.0F);
        add_param("gain", 1.0F);
        const auto it = plugin.tables.find(def.table_file);
        table_ = it != plugin.tables.end() ? &it->second : nullptr;
        break;
    }
    case ntp::NodeType::kConvolver: {
        const auto it = plugin.samples.find(def.impulse);
        if (it != plugin.samples.end()) {
            const audio::SampleBuffer& ir = *it->second;
            const std::uint32_t taps = std::min(ir.frames, kMaxImpulseFrames);
            ir_l_.resize(taps);
            ir_r_.resize(taps);
            for (std::uint32_t i = 0; i < taps; ++i) {
                ir_l_[i] = ir.interleaved[static_cast<std::size_t>(i) * 2];
                ir_r_[i] = ir.interleaved[(static_cast<std::size_t>(i) * 2) + 1];
            }
            if (def.normalize) {
                double energy = 0.0;
                for (std::uint32_t i = 0; i < taps; ++i) {
                    energy += (static_cast<double>(ir_l_[i]) * ir_l_[i]) +
                              (static_cast<double>(ir_r_[i]) * ir_r_[i]);
                }
                const float scale =
                    energy > 0.0 ? static_cast<float>(1.0 / std::sqrt(energy)) : 1.0F;
                for (std::uint32_t i = 0; i < taps; ++i) {
                    ir_l_[i] *= scale;
                    ir_r_[i] *= scale;
                }
            }
            history_l_.assign(taps, 0.0F);
            history_r_.assign(taps, 0.0F);
        }
        break;
    }
    case ntp::NodeType::kSampler: {
        sampler_voices_.resize(static_cast<std::size_t>(std::clamp(def.polyphony, 1, 64)));
        zone_buffers_.reserve(def.zones.size());
        rr_group_of_zone_.reserve(def.zones.size());
        choke_group_of_zone_.reserve(def.zones.size());
        std::vector<std::string> rr_names;
        std::vector<std::string> choke_names;
        for (const ntp::SampleZone& zone : def.zones) {
            // Baked default: user-assignable zones play their fallback
            // until an override lands; plain zones play `file`.
            const std::string& baked = zone.user_assignable && !zone.fallback_file.empty()
                                           ? zone.fallback_file
                                           : zone.file;
            const auto it = plugin.samples.find(baked);
            zone_buffers_.push_back(it != plugin.samples.end() ? it->second.get() : nullptr);
            rr_group_of_zone_.push_back(intern_group(rr_names, zone.round_robin_group));
            choke_group_of_zone_.push_back(intern_group(choke_names, zone.choke_group));
        }
        zone_override_ = std::vector<std::atomic<const audio::SampleBuffer*>>(def.zones.size());
        rr_counters_.assign(rr_names.size(), 0);
        rr_advanced_.assign(rr_names.size(), 0);

        if (def.slice_map.has_value()) {
            const ntp::SliceMap& map = *def.slice_map;
            // Baked slice source: an archive path directly, or the
            // fallback of the referenced user-assignable slot (which
            // may live on another sampler node — slot ids are
            // plugin-wide).
            std::string source_path = map.source;
            if (source_path.starts_with("slotId:")) {
                const std::string slot = source_path.substr(7);
                source_path.clear();
                for (const ntp::DspNode& other : plugin.manifest.graph.nodes) {
                    for (const ntp::SampleZone& zone : other.zones) {
                        if (zone.user_assignable && zone.slot_id == slot) {
                            source_path =
                                zone.fallback_file.empty() ? zone.file : zone.fallback_file;
                        }
                    }
                }
            }
            const auto it = plugin.samples.find(source_path);
            slice_buffer_ = it != plugin.samples.end() ? it->second.get() : nullptr;

            std::vector<std::string> slice_rr_names;
            slice_note_.reserve(map.slices.size());
            slice_choke_of_.reserve(map.slices.size());
            slice_rr_of_.reserve(map.slices.size());
            slice_cut_on_off_.reserve(map.slices.size());
            for (std::size_t s = 0; s < map.slices.size(); ++s) {
                const ntp::SliceEntry& slice = map.slices[s];
                slice_note_.push_back(slice.note >= 0 ? slice.note
                                                      : ntp::kSliceBaseNote + static_cast<int>(s));
                slice_choke_of_.push_back(intern_group(choke_names, slice.choke_group));
                slice_rr_of_.push_back(intern_group(slice_rr_names, slice.round_robin_group));
                slice_cut_on_off_.push_back(
                    map.trigger_mode == ntp::SliceTriggerMode::kGated && slice.release_on_gate ? 1
                                                                                               : 0);
            }
            slice_rr_counters_.assign(slice_rr_names.size(), 0);
            slice_rr_advanced_.assign(slice_rr_names.size(), 0);
        }
        break;
    }
    case ntp::NodeType::kNativeStage:
        break; // never instantiated — the loader refuses it
    }
}

ParamSlot* NodeRuntime::find_param(const char* name) {
    for (int i = 0; i < param_count_; ++i) {
        if (std::strcmp(params_[static_cast<std::size_t>(i)].name, name) == 0) {
            return &params_[static_cast<std::size_t>(i)];
        }
    }
    return nullptr;
}

float NodeRuntime::resolved(int slot, std::uint32_t frame) const {
    const ParamSlot& p = params_[static_cast<std::size_t>(slot)];
    float value = p.base + p.mod;
    if (p.audio_mod != nullptr) {
        value += downmix(p.audio_mod, frame);
    }
    return value;
}

void NodeRuntime::note_on() {
    if (def_->type == ntp::NodeType::kEnvelope) {
        env_stage_ = 0;
        env_stage_t_ = 0.0;
        env_stage_from_ = env_value_;
        env_released_ = false;
    }
}

bool NodeRuntime::envelope_active() const {
    if (def_->type != ntp::NodeType::kEnvelope) {
        return false;
    }
    return !env_released_ || env_value_ > 1e-4F;
}

void NodeRuntime::process_envelope(float* out, std::uint32_t frames,
                                   const VoiceControls* controls) {
    const float gate = controls != nullptr ? controls->gate : 1.0F;
    const float release_time = std::max(1e-3F, resolved(0, 0));
    const auto& stages = def_->env_stages;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (gate < 0.5F && !env_released_) {
            env_released_ = true;
            env_release_from_ = env_value_;
            env_release_t_ = 0.0;
        }
        if (env_released_) {
            env_release_t_ += 1.0 / rate_;
            const float t = static_cast<float>(env_release_t_) / release_time;
            env_value_ = t >= 1.0F ? 0.0F : env_release_from_ * (1.0F - t);
        } else if (env_stage_ >= 0 && env_stage_ < static_cast<int>(stages.size())) {
            const ntp::EnvelopeStage& stage = stages[static_cast<std::size_t>(env_stage_)];
            env_stage_t_ += 1.0 / rate_;
            const double duration = std::max(1e-4, stage.time);
            double t = env_stage_t_ / duration;
            if (t >= 1.0) {
                env_value_ = static_cast<float>(stage.target);
                ++env_stage_;
                if (env_stage_ >= static_cast<int>(stages.size())) {
                    env_stage_ = -1; // hold the final level (sustain)
                }
                env_stage_t_ = 0.0;
                env_stage_from_ = env_value_;
            } else {
                if (stage.exponential) {
                    t = 1.0 - std::pow(1.0 - t, 3.0);
                }
                env_value_ =
                    env_stage_from_ +
                    (static_cast<float>(stage.target) - env_stage_from_) * static_cast<float>(t);
            }
        }
        out[static_cast<std::size_t>(i) * 2] = env_value_;
        out[(static_cast<std::size_t>(i) * 2) + 1] = env_value_;
    }
}

void NodeRuntime::process_granular(float* out, std::uint32_t frames,
                                   const VoiceControls* controls) {
    std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
    if (source_buffer_ == nullptr || source_buffer_->frames == 0) {
        return;
    }
    // k-rate params (indices match the constructor's add_param order).
    const float density = std::max(0.1F, resolved(1, 0));
    const float scan_rate = resolved(4, 0);
    const float position_jitter = resolved(7, 0);
    const float pitch_jitter = resolved(8, 0);
    const float size_jitter = resolved(9, 0);
    const float pan_jitter = resolved(10, 0);
    const bool freeze = def_->playback_mode == "freeze";

    if (!freeze) {
        scan_pos_ += static_cast<double>(scan_rate) * frames / rate_;
        scan_pos_ -= std::floor(scan_pos_);
    }
    const double samples_per_spawn = static_cast<double>(rate_) / density;
    const std::uint32_t buffer_len = source_buffer_->frames;
    const float* src = source_buffer_->interleaved.data();

    for (std::uint32_t i = 0; i < frames; ++i) {
        const float position = resolved(0, i);
        const float grain_size = resolved(2, i);
        const float pitch = resolved(3, i);
        const float pan = resolved(5, i);
        const float gate =
            controls != nullptr ? std::min(controls->gate, resolved(6, i)) : resolved(6, i);

        if (gate > 0.5F) {
            spawn_counter_ -= 1.0;
            if (spawn_counter_ <= 0.0) {
                const float j_pos = position + ((rng01(rng_) - 0.5F) * position_jitter);
                const float j_pitch = pitch + ((rng01(rng_) - 0.5F) * pitch_jitter * 0.01F);
                const float j_size =
                    std::max(0.005F, grain_size * (1.0F + ((rng01(rng_) - 0.5F) * size_jitter)));
                const float j_pan =
                    std::clamp(pan + ((rng01(rng_) - 0.5F) * pan_jitter * 2.0F), -1.0F, 1.0F);
                // Spawn into the first free slot; pool-full drops.
                for (Grain& grain : grains_) {
                    if (grain.active) {
                        continue;
                    }
                    const float wrapped = j_pos + static_cast<float>(scan_pos_) -
                                          std::floor(j_pos + static_cast<float>(scan_pos_));
                    grain.sample_pos = static_cast<double>(wrapped) * buffer_len;
                    double rate = std::pow(2.0, static_cast<double>(j_pitch) / 12.0);
                    if (def_->playback_mode == "reverse" ||
                        (def_->playback_mode == "pingpong" && rng01(rng_) < 0.5F)) {
                        rate = -rate;
                    }
                    grain.play_rate = rate;
                    grain.age = 0;
                    grain.age_max = std::max(
                        8U, static_cast<std::uint32_t>(static_cast<double>(j_size) * rate_));
                    const float angle = (j_pan + 1.0F) * (std::numbers::pi_v<float> / 4.0F);
                    grain.pan_l = std::cos(angle);
                    grain.pan_r = std::sin(angle);
                    grain.active = true;
                    break;
                }
                spawn_counter_ = samples_per_spawn;
            }
        }

        float mix_l = 0.0F;
        float mix_r = 0.0F;
        for (Grain& grain : grains_) {
            if (!grain.active) {
                continue;
            }
            if (grain.age >= grain.age_max) {
                grain.active = false;
                continue;
            }
            const float t = static_cast<float>(grain.age) / static_cast<float>(grain.age_max);
            float env = 0.5F * (1.0F - std::cos(2.0F * std::numbers::pi_v<float> * t));
            if (def_->grain_envelope == "triangle") {
                env = 1.0F - std::abs((2.0F * t) - 1.0F);
            } else if (def_->grain_envelope == "rectangular") {
                env = 1.0F;
            }
            double idx = std::fmod(grain.sample_pos, static_cast<double>(buffer_len));
            if (idx < 0.0) {
                idx += buffer_len;
            }
            const auto lo = static_cast<std::uint32_t>(idx);
            const auto frac = static_cast<float>(idx - lo);
            const auto hi = (lo + 1) % buffer_len;
            const float s_l =
                src[static_cast<std::size_t>(lo) * 2] +
                ((src[static_cast<std::size_t>(hi) * 2] - src[static_cast<std::size_t>(lo) * 2]) *
                 frac);
            const float s_r = src[(static_cast<std::size_t>(lo) * 2) + 1] +
                              ((src[(static_cast<std::size_t>(hi) * 2) + 1] -
                                src[(static_cast<std::size_t>(lo) * 2) + 1]) *
                               frac);
            mix_l += s_l * env * grain.pan_l;
            mix_r += s_r * env * grain.pan_r;
            grain.sample_pos += grain.play_rate;
            ++grain.age;
        }
        // The worklet's conservative 0.5 headroom attenuation.
        out[static_cast<std::size_t>(i) * 2] = mix_l * 0.5F;
        out[(static_cast<std::size_t>(i) * 2) + 1] = mix_r * 0.5F;
    }
}

void NodeRuntime::process_wavetable(float* out, std::uint32_t frames) {
    std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
    if (table_ == nullptr || table_->mono.empty() || def_->frame_count < 1) {
        return;
    }
    const int frame_count = std::max(1, def_->frame_count);
    const std::size_t frame_size =
        std::max<std::size_t>(1, table_->mono.size() / static_cast<std::size_t>(frame_count));
    const int last_frame = frame_count - 1;
    const float* table = table_->mono.data();

    auto read_sample = [&](int frame_index, double phase) {
        const auto offset = static_cast<std::size_t>(frame_index) * frame_size;
        const double f_idx = phase * static_cast<double>(frame_size);
        const auto lo = static_cast<std::size_t>(f_idx);
        if (!def_->interpolation_linear) {
            return table[offset + (lo % frame_size)];
        }
        const auto frac = static_cast<float>(f_idx - static_cast<double>(lo));
        const float a = table[offset + (lo % frame_size)];
        const float b = table[offset + ((lo + 1) % frame_size)];
        return a + ((b - a) * frac);
    };

    for (std::uint32_t i = 0; i < frames; ++i) {
        const float freq_base = resolved(0, i);
        const float frame_pos = std::clamp(resolved(1, i), 0.0F, 1.0F);
        const float detune = resolved(2, i);
        const float gain = resolved(3, i);

        const double freq =
            static_cast<double>(freq_base) * std::pow(2.0, static_cast<double>(detune) / 1200.0);
        const double f_pos = static_cast<double>(frame_pos) * last_frame;
        const int lo = static_cast<int>(f_pos);
        const int hi = std::min(last_frame, lo + 1);
        const auto f_frac = static_cast<float>(f_pos - lo);

        const float sample = read_sample(lo, table_phase_) * (1.0F - f_frac) +
                             (read_sample(hi, table_phase_) * f_frac);
        const float v = sample * gain;
        out[static_cast<std::size_t>(i) * 2] = v;
        out[(static_cast<std::size_t>(i) * 2) + 1] = v;

        table_phase_ += freq / rate_;
        table_phase_ -= std::floor(table_phase_);
    }
}

NodeRuntime::SamplerVoice* NodeRuntime::allocate_sampler_voice() {
    for (SamplerVoice& voice : sampler_voices_) {
        if (!voice.active) {
            return &voice;
        }
    }
    if (def_->voice_stealing == "none") {
        return nullptr;
    }
    SamplerVoice* slot = sampler_voices_.data();
    for (SamplerVoice& voice : sampler_voices_) {
        const bool older = voice.age < slot->age;
        const bool quieter = voice.gain < slot->gain;
        if (def_->voice_stealing == "quietest" ? quieter : older) {
            slot = &voice;
        }
    }
    return slot;
}

void NodeRuntime::choke_sampler_voices(int choke_group) {
    for (SamplerVoice& voice : sampler_voices_) {
        if (voice.active && voice.choke == choke_group) {
            voice.active = false;
        }
    }
}

void NodeRuntime::sampler_note_on(int note, float velocity) {
    const int vel_127 = std::clamp(static_cast<int>(velocity * 127.0F), 1, 127);
    for (std::size_t z = 0; z < def_->zones.size(); ++z) {
        const ntp::SampleZone& zone = def_->zones[z];
        // User override first, baked archive sample (fallbackFile for
        // user-assignable zones) second.
        const audio::SampleBuffer* override_buffer =
            zone_override_[z].load(std::memory_order_acquire);
        const audio::SampleBuffer* buffer =
            override_buffer != nullptr ? override_buffer : zone_buffers_[z];
        if (buffer == nullptr || zone.release_trigger) {
            continue;
        }
        if (note < zone.key_lo || note > zone.key_hi || vel_127 < zone.vel_lo ||
            vel_127 > zone.vel_hi) {
            continue;
        }
        // Round-robin: only the group's current pick fires this trigger.
        const int rr = rr_group_of_zone_[z];
        if (rr >= 0) {
            int members = 0;
            int my_index = 0;
            for (std::size_t other = 0; other < def_->zones.size(); ++other) {
                if (rr_group_of_zone_[other] == rr) {
                    if (other == z) {
                        my_index = members;
                    }
                    ++members;
                }
            }
            if (members > 1 && (rr_counters_[static_cast<std::size_t>(rr)] % members) != my_index) {
                continue;
            }
        }
        // Choke: cut same-group voices (zone or slice alike).
        const int choke = choke_group_of_zone_[z];
        if (choke >= 0) {
            choke_sampler_voices(choke);
        }
        SamplerVoice* slot = allocate_sampler_voice();
        if (slot == nullptr) {
            continue; // stealing policy "none" with a full pool
        }
        const auto rate_frames = static_cast<double>(buffer->rate);
        slot->buffer = buffer;
        slot->zone = &def_->zones[z];
        slot->pos = zone.start_offset * rate_frames;
        slot->step = zone.pitch_tracking
                         ? std::pow(2.0, static_cast<double>(note - zone.root_key) / 12.0)
                         : 1.0;
        slot->direction = 1;
        slot->loop_start = zone.loop_start * rate_frames;
        slot->loop_end = zone.loop_end > 0.0 ? zone.loop_end * rate_frames : buffer->frames;
        slot->end_frame = zone.duration > 0.0
                              ? std::min<double>(buffer->frames,
                                                 (zone.start_offset + zone.duration) * rate_frames)
                              : buffer->frames;
        slot->gain = velocity;
        slot->note = note;
        slot->choke = choke;
        slot->slice_index = -1;
        slot->released = false;
        slot->age = ++sampler_clock_;
        slot->active = true;
        break; // one zone per trigger (after RR/choke resolution)
    }
    // Advance every matching round-robin group counter once per trigger.
    std::fill(rr_advanced_.begin(), rr_advanced_.end(), std::uint8_t{0});
    for (std::size_t z = 0; z < def_->zones.size(); ++z) {
        const ntp::SampleZone& zone = def_->zones[z];
        const int rr = rr_group_of_zone_[z];
        if (rr < 0 || rr_advanced_[static_cast<std::size_t>(rr)] != 0) {
            continue;
        }
        if (note >= zone.key_lo && note <= zone.key_hi && vel_127 >= zone.vel_lo &&
            vel_127 <= zone.vel_hi) {
            ++rr_counters_[static_cast<std::size_t>(rr)];
            rr_advanced_[static_cast<std::size_t>(rr)] = 1;
        }
    }

    // ── Slices ───────────────────────────────────────────────────────
    // Same trigger discipline as zones: note match, RR gate, choke cut,
    // one slice per trigger. Chops play at source pitch (no tracking).
    if (def_->slice_map.has_value() && !slice_note_.empty()) {
        const audio::SampleBuffer* override_buffer =
            slice_override_.load(std::memory_order_acquire);
        const audio::SampleBuffer* buffer =
            override_buffer != nullptr ? override_buffer : slice_buffer_;
        if (buffer == nullptr) {
            return;
        }
        const std::vector<ntp::SliceEntry>& slices = def_->slice_map->slices;
        for (std::size_t s = 0; s < slices.size(); ++s) {
            if (slice_note_[s] != note || vel_127 < slices[s].velocity_min) {
                continue;
            }
            const int rr = slice_rr_of_[s];
            if (rr >= 0) {
                int members = 0;
                int my_index = 0;
                for (std::size_t other = 0; other < slices.size(); ++other) {
                    if (slice_rr_of_[other] == rr) {
                        if (other == s) {
                            my_index = members;
                        }
                        ++members;
                    }
                }
                if (members > 1 &&
                    (slice_rr_counters_[static_cast<std::size_t>(rr)] % members) != my_index) {
                    continue;
                }
            }
            const int choke = slice_choke_of_[s];
            if (choke >= 0) {
                choke_sampler_voices(choke);
            }
            SamplerVoice* slot = allocate_sampler_voice();
            if (slot == nullptr) {
                continue;
            }
            const auto rate_frames = static_cast<double>(buffer->rate);
            slot->buffer = buffer;
            slot->zone = nullptr;
            slot->pos = slices[s].start * rate_frames;
            slot->step = 1.0;
            slot->direction = 1;
            slot->loop_start = 0.0;
            slot->loop_end = 0.0;
            slot->end_frame = std::min<double>(buffer->frames, slices[s].end * rate_frames);
            slot->gain = velocity;
            slot->note = note;
            slot->choke = choke;
            slot->slice_index = static_cast<int>(s);
            slot->released = false;
            slot->age = ++sampler_clock_;
            slot->active = true;
            break;
        }
        std::fill(slice_rr_advanced_.begin(), slice_rr_advanced_.end(), std::uint8_t{0});
        for (std::size_t s = 0; s < slices.size(); ++s) {
            const int rr = slice_rr_of_[s];
            if (rr < 0 || slice_rr_advanced_[static_cast<std::size_t>(rr)] != 0) {
                continue;
            }
            if (slice_note_[s] == note && vel_127 >= slices[s].velocity_min) {
                ++slice_rr_counters_[static_cast<std::size_t>(rr)];
                slice_rr_advanced_[static_cast<std::size_t>(rr)] = 1;
            }
        }
    }
}

void NodeRuntime::sampler_note_off(int note) {
    for (SamplerVoice& voice : sampler_voices_) {
        if (voice.active && voice.note == note) {
            voice.released = true;
            if (voice.zone != nullptr && voice.zone->loop == ntp::LoopMode::kRelease) {
                voice.active = false; // release loops end the sustain portion
            }
            // Gated slices cut on noteOff (per-slice releaseOnGate
            // opt-out); one-shot slices play out.
            if (voice.slice_index >= 0 &&
                slice_cut_on_off_[static_cast<std::size_t>(voice.slice_index)] != 0) {
                voice.active = false;
            }
        }
    }
    // Release-triggered zones fire now.
    const int vel_127 = 100;
    for (std::size_t z = 0; z < def_->zones.size(); ++z) {
        const ntp::SampleZone& zone = def_->zones[z];
        const audio::SampleBuffer* override_buffer =
            zone_override_[z].load(std::memory_order_acquire);
        const audio::SampleBuffer* buffer =
            override_buffer != nullptr ? override_buffer : zone_buffers_[z];
        if (buffer == nullptr || !zone.release_trigger) {
            continue;
        }
        if (note < zone.key_lo || note > zone.key_hi || vel_127 < zone.vel_lo ||
            vel_127 > zone.vel_hi) {
            continue;
        }
        for (SamplerVoice& voice : sampler_voices_) {
            if (voice.active) {
                continue;
            }
            const auto rate_frames = static_cast<double>(buffer->rate);
            voice.buffer = buffer;
            voice.zone = &zone;
            voice.pos = zone.start_offset * rate_frames;
            voice.step = zone.pitch_tracking
                             ? std::pow(2.0, static_cast<double>(note - zone.root_key) / 12.0)
                             : 1.0;
            voice.direction = 1;
            voice.loop_start = 0.0;
            voice.loop_end = 0.0;
            voice.end_frame = buffer->frames;
            voice.gain = 0.8F;
            voice.note = note;
            voice.choke = choke_group_of_zone_[z];
            voice.slice_index = -1;
            voice.released = true;
            voice.age = ++sampler_clock_;
            voice.active = true;
            break;
        }
    }
}

void NodeRuntime::sampler_reset() {
    for (SamplerVoice& voice : sampler_voices_) {
        voice.active = false;
        voice.buffer = nullptr;
    }
}

void NodeRuntime::set_slot_override_buffer(const std::string& slot_id,
                                           const audio::SampleBuffer* buffer) {
    if (def_->type != ntp::NodeType::kSampler) {
        return;
    }
    for (std::size_t z = 0; z < def_->zones.size(); ++z) {
        const ntp::SampleZone& zone = def_->zones[z];
        if (zone.user_assignable && zone.slot_id == slot_id) {
            zone_override_[z].store(buffer, std::memory_order_release);
        }
    }
    // A slot-sourced slice map retargets with its slot, even when the
    // owning zone lives on another sampler node.
    if (def_->slice_map.has_value() && def_->slice_map->source == "slotId:" + slot_id) {
        slice_override_.store(buffer, std::memory_order_release);
    }
}

bool NodeRuntime::sampler_active() const {
    return std::any_of(sampler_voices_.begin(), sampler_voices_.end(),
                       [](const SamplerVoice& voice) { return voice.active; });
}

void NodeRuntime::process_sampler(float* out, std::uint32_t frames) {
    std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
    for (SamplerVoice& voice : sampler_voices_) {
        if (!voice.active || voice.buffer == nullptr) {
            continue;
        }
        const audio::SampleBuffer& buffer = *voice.buffer;
        const bool looping = voice.zone != nullptr &&
                             (voice.zone->loop == ntp::LoopMode::kForward ||
                              voice.zone->loop == ntp::LoopMode::kPingpong) &&
                             voice.loop_end > voice.loop_start && !voice.released;
        for (std::uint32_t i = 0; i < frames; ++i) {
            if (looping) {
                if (voice.zone->loop == ntp::LoopMode::kPingpong) {
                    if (voice.direction > 0 && voice.pos >= voice.loop_end) {
                        voice.direction = -1;
                        voice.pos = voice.loop_end;
                    } else if (voice.direction < 0 && voice.pos <= voice.loop_start) {
                        voice.direction = 1;
                        voice.pos = voice.loop_start;
                    }
                } else if (voice.pos >= voice.loop_end) {
                    voice.pos = voice.loop_start + (voice.pos - voice.loop_end);
                }
            }
            const auto idx = static_cast<std::uint32_t>(voice.pos);
            if (voice.pos < 0.0 || idx + 1 >= static_cast<std::uint32_t>(voice.end_frame) ||
                idx + 1 >= buffer.frames) {
                voice.active = false;
                break;
            }
            const auto frac = static_cast<float>(voice.pos - idx);
            const auto s0 = static_cast<std::size_t>(idx) * 2;
            const float l = buffer.interleaved[s0] +
                            ((buffer.interleaved[s0 + 2] - buffer.interleaved[s0]) * frac);
            const float r = buffer.interleaved[s0 + 1] +
                            ((buffer.interleaved[s0 + 3] - buffer.interleaved[s0 + 1]) * frac);
            out[static_cast<std::size_t>(i) * 2] += l * voice.gain;
            out[(static_cast<std::size_t>(i) * 2) + 1] += r * voice.gain;
            voice.pos += voice.step * voice.direction;
        }
    }
}

void NodeRuntime::process(const float* in, float* out, std::uint32_t frames,
                          const VoiceControls* controls) {
    switch (def_->type) {
    case ntp::NodeType::kGain:
    case ntp::NodeType::kMixer:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float g = resolved(0, i);
            out[static_cast<std::size_t>(i) * 2] = in[static_cast<std::size_t>(i) * 2] * g;
            out[(static_cast<std::size_t>(i) * 2) + 1] =
                in[(static_cast<std::size_t>(i) * 2) + 1] * g;
        }
        break;
    case ntp::NodeType::kDelay:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float delay_frames = std::max(1.0F, resolved(0, i) * static_cast<float>(rate_));
            delay_l_.write(in[static_cast<std::size_t>(i) * 2]);
            delay_r_.write(in[(static_cast<std::size_t>(i) * 2) + 1]);
            out[static_cast<std::size_t>(i) * 2] = delay_l_.read(delay_frames);
            out[(static_cast<std::size_t>(i) * 2) + 1] = delay_r_.read(delay_frames);
        }
        break;
    case ntp::NodeType::kBiquad: {
        const float freq = std::clamp(resolved(0, 0), 10.0F, static_cast<float>(rate_) * 0.45F);
        const float q = std::max(0.05F, resolved(1, 0));
        if (!biquad_configured_ || freq != biquad_last_freq_ || q != biquad_last_q_) {
            biquad_l_.configure(biquad_type_from(def_->filter_type), freq, q, rate_);
            biquad_r_.configure(biquad_type_from(def_->filter_type), freq, q, rate_);
            biquad_configured_ = true;
            biquad_last_freq_ = freq;
            biquad_last_q_ = q;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            out[static_cast<std::size_t>(i) * 2] =
                biquad_l_.process(in[static_cast<std::size_t>(i) * 2]);
            out[(static_cast<std::size_t>(i) * 2) + 1] =
                biquad_r_.process(in[(static_cast<std::size_t>(i) * 2) + 1]);
        }
        break;
    }
    case ntp::NodeType::kCompressor: {
        if (!compressor_configured_ || params_[0].mod != 0.0F || params_[1].mod != 0.0F) {
            compressor_.set(resolved(0, 0), std::max(1.0F, resolved(1, 0)),
                            std::max(1e-4F, resolved(2, 0)), std::max(1e-3F, resolved(3, 0)),
                            resolved(4, 0));
            compressor_configured_ = true;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::size_t at = static_cast<std::size_t>(i) * 2;
            const float g = compressor_.gain_for(in[at], in[at + 1]);
            out[at] = in[at] * g;
            out[at + 1] = in[at + 1] * g;
        }
        break;
    }
    case ntp::NodeType::kPanner:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float pan = std::clamp(resolved(0, i), -1.0F, 1.0F);
            const float angle = (pan + 1.0F) * (std::numbers::pi_v<float> / 4.0F);
            const std::size_t at = static_cast<std::size_t>(i) * 2;
            const float mono = (in[at] + in[at + 1]) * 0.5F;
            out[at] = mono * std::cos(angle) * std::numbers::sqrt2_v<float>;
            out[at + 1] = mono * std::sin(angle) * std::numbers::sqrt2_v<float>;
        }
        break;
    case ntp::NodeType::kWaveshaper:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float drive = std::max(0.01F, resolved(0, i));
            const std::size_t at = static_cast<std::size_t>(i) * 2;
            for (int ch = 0; ch < 2; ++ch) {
                const float x = in[at + static_cast<std::size_t>(ch)] * drive;
                float y = 0.0F;
                if (def_->shaper_curve == "clip") {
                    y = std::clamp(x, -1.0F, 1.0F);
                } else if (def_->shaper_curve == "fold") {
                    y = std::abs(std::fmod(x + 1.0F, 4.0F) - 2.0F) - 1.0F;
                } else { // sigmoid
                    y = std::tanh(x);
                }
                out[at + static_cast<std::size_t>(ch)] = y;
            }
        }
        break;
    case ntp::NodeType::kOscillator:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const double freq = static_cast<double>(resolved(0, i)) *
                                std::pow(2.0, static_cast<double>(resolved(1, i)) / 1200.0);
            const float gain = resolved(2, i);
            const auto phase = static_cast<float>(osc_phase_);
            float v = 0.0F;
            if (def_->osc_type == "square") {
                v = phase < 0.5F ? 1.0F : -1.0F;
            } else if (def_->osc_type == "sawtooth") {
                v = (2.0F * phase) - 1.0F;
            } else if (def_->osc_type == "triangle") {
                v = 1.0F - (4.0F * std::abs(phase - 0.5F));
            } else { // sine
                v = std::sin(2.0F * std::numbers::pi_v<float> * phase);
            }
            v *= gain;
            out[static_cast<std::size_t>(i) * 2] = v;
            out[(static_cast<std::size_t>(i) * 2) + 1] = v;
            osc_phase_ += freq / rate_;
            osc_phase_ -= std::floor(osc_phase_);
        }
        break;
    case ntp::NodeType::kNoise:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float white = (rng01(rng_) * 2.0F) - 1.0F;
            float v = white;
            if (def_->noise_type == "pink") {
                // Paul Kellet's economy pink filter.
                pink_[0] = (0.99765F * pink_[0]) + (white * 0.0990460F);
                pink_[1] = (0.96300F * pink_[1]) + (white * 0.2965164F);
                pink_[2] = (0.57000F * pink_[2]) + (white * 1.0526913F);
                v = (pink_[0] + pink_[1] + pink_[2] + (white * 0.1848F)) * 0.2F;
            } else if (def_->noise_type == "brown") {
                brown_ = std::clamp((brown_ + (0.02F * white)) / 1.02F, -1.0F, 1.0F);
                v = brown_ * 3.5F;
            }
            v *= resolved(0, i);
            out[static_cast<std::size_t>(i) * 2] = v;
            out[(static_cast<std::size_t>(i) * 2) + 1] = v;
        }
        break;
    case ntp::NodeType::kConstant:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float v = resolved(0, i);
            out[static_cast<std::size_t>(i) * 2] = v;
            out[(static_cast<std::size_t>(i) * 2) + 1] = v;
        }
        break;
    case ntp::NodeType::kLfo:
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float rate_hz = std::max(0.0F, resolved(0, i));
            const float depth = resolved(1, i);
            const auto phase = static_cast<float>(lfo_phase_);
            float v = 0.0F;
            if (def_->lfo_shape == "triangle") {
                v = 1.0F - (4.0F * std::abs(phase - 0.5F));
            } else if (def_->lfo_shape == "square") {
                v = phase < 0.5F ? 1.0F : -1.0F;
            } else if (def_->lfo_shape == "sawtooth") {
                v = (2.0F * phase) - 1.0F;
            } else if (def_->lfo_shape == "sample-and-hold") {
                v = sh_value_;
            } else { // sine
                v = std::sin(2.0F * std::numbers::pi_v<float> * phase);
            }
            v *= depth;
            out[static_cast<std::size_t>(i) * 2] = v;
            out[(static_cast<std::size_t>(i) * 2) + 1] = v;
            const double before = lfo_phase_;
            lfo_phase_ += static_cast<double>(rate_hz) / rate_;
            if (lfo_phase_ >= 1.0) {
                lfo_phase_ -= std::floor(lfo_phase_);
                if (before < 1.0) {
                    sh_value_ = (rng01(rng_) * 2.0F) - 1.0F;
                }
            }
        }
        break;
    case ntp::NodeType::kEnvelope:
        process_envelope(out, frames, controls);
        break;
    case ntp::NodeType::kGranular:
        process_granular(out, frames, controls);
        break;
    case ntp::NodeType::kWavetable:
        process_wavetable(out, frames);
        break;
    case ntp::NodeType::kConvolver: {
        const std::size_t taps = ir_l_.size();
        if (taps == 0) {
            std::copy_n(in, static_cast<std::size_t>(frames) * 2, out);
            break;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            history_l_[history_pos_] = in[static_cast<std::size_t>(i) * 2];
            history_r_[history_pos_] = in[(static_cast<std::size_t>(i) * 2) + 1];
            float acc_l = 0.0F;
            float acc_r = 0.0F;
            std::size_t h = history_pos_;
            for (std::size_t t = 0; t < taps; ++t) {
                acc_l += ir_l_[t] * history_l_[h];
                acc_r += ir_r_[t] * history_r_[h];
                h = h == 0 ? taps - 1 : h - 1;
            }
            out[static_cast<std::size_t>(i) * 2] = acc_l;
            out[(static_cast<std::size_t>(i) * 2) + 1] = acc_r;
            history_pos_ = (history_pos_ + 1) % taps;
        }
        break;
    }
    case ntp::NodeType::kSampler:
        process_sampler(out, frames);
        break;
    case ntp::NodeType::kNativeStage:
        std::fill_n(out, static_cast<std::size_t>(frames) * 2, 0.0F);
        break;
    }
}

} // namespace nt::plugins
