// plugins/ntp_graph — materialisation of an NTP manifest graph into
// processable node runtimes. One NodeRuntime per manifest node per
// instantiation (shared nodes once, voice nodes once per pool voice);
// all state is preallocated at build so process() is RT-safe.
//
// Signal model: stereo interleaved blocks of up to kNtpBlockFrames.
// Every node has one signal input sum and one output block. Parameters
// resolve per block as base + audio-rate param connections + k-rate mod
// routes (slewed); the resolved value is a per-block mod buffer so FM
// and audio-rate modulation keep WebAudio's semantics.
//
// The granular and wavetable nodes are direct ports of the web's
// host-shipped AudioWorklets (public/audioworklets/granular.js,
// wavetable.js) — grain pool sizes, jitter behaviour, envelope shapes
// and parameter sets preserved. The convolver is direct-form FIR with
// the impulse capped at kMaxImpulseFrames; long-tail convolution waits
// on a partitioned-FFT engine (recorded in FIXES.md).
#pragma once

#include "audio/dsp.h"
#include "ntp/ntp_manifest.h"
#include "plugins/ntp_loader.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace nt::plugins {

inline constexpr std::uint32_t kNtpBlockFrames = 128;
inline constexpr std::uint32_t kMaxImpulseFrames = 2048;
inline constexpr int kMaxGrains = 128;

// Per-voice control signals (constant per block).
struct VoiceControls {
    float pitch_hz = 440.0F;
    float note = 69.0F;    // MIDI
    float velocity = 1.0F; // 0..1
    float gate = 0.0F;     // 1 while held
};

// A modulatable parameter slot on a node runtime. `base` comes from the
// manifest (or a dot-path host parameter); mod routes and audio-rate
// param connections add on top per block.
struct ParamSlot {
    const char* name = "";
    float base = 0.0F;
    float mod = 0.0F;                 // k-rate contribution this block
    const float* audio_mod = nullptr; // stereo block driving this param, or null
};

inline constexpr int kMaxParamSlots = 12;

class NodeRuntime {
public:
    NodeRuntime(const ntp::DspNode& def, const LoadedNtpPlugin& plugin, std::uint32_t rate,
                std::uint32_t seed);

    // Renders `frames` into out (stereo interleaved) from the summed
    // input. `controls` supplies the per-voice signals (null for
    // shared/FX nodes — gate defaults open so free-running sources
    // sound).
    void process(const float* in, float* out, std::uint32_t frames, const VoiceControls* controls);

    // Restarts voice-scoped state at noteOn (envelope stages, phases
    // stay free-running for oscillators — classic analogue behaviour).
    void note_on();

    [[nodiscard]] const ntp::DspNode& def() const { return *def_; }

    // True while the node still shapes audible output after gate-off
    // (envelope tail). Voice-end detection asks every envelope node.
    [[nodiscard]] bool envelope_active() const;

    ParamSlot* find_param(const char* name);

    [[nodiscard]] std::array<ParamSlot, kMaxParamSlots>& params() { return params_; }

private:
    [[nodiscard]] float resolved(int slot, std::uint32_t frame) const;

    const ntp::DspNode* def_;
    std::uint32_t rate_;
    std::array<ParamSlot, kMaxParamSlots> params_{};
    int param_count_ = 0;

    // ── Per-type state ───────────────────────────────────────────────
    audio::dsp::Biquad biquad_l_, biquad_r_;
    bool biquad_configured_ = false;
    float biquad_last_freq_ = -1.0F, biquad_last_q_ = -1.0F;
    audio::dsp::DelayLine delay_l_, delay_r_;
    audio::dsp::Compressor compressor_;
    bool compressor_configured_ = false;
    double osc_phase_ = 0.0;
    std::uint32_t rng_;
    std::array<float, 3> pink_{};
    float brown_ = 0.0F;
    double lfo_phase_ = 0.0;
    float sh_value_ = 0.0F;
    // envelope
    int env_stage_ = -1;
    double env_stage_t_ = 0.0;
    float env_value_ = 0.0F;
    float env_stage_from_ = 0.0F;
    bool env_released_ = false;
    float env_release_from_ = 0.0F;
    double env_release_t_ = 0.0;

    // granular
    struct Grain {
        bool active = false;
        double sample_pos = 0.0;
        double play_rate = 1.0;
        std::uint32_t age = 0;
        std::uint32_t age_max = 1;
        float pan_l = 0.7071F;
        float pan_r = 0.7071F;
    };

    std::vector<Grain> grains_;
    double spawn_counter_ = 0.0;
    double scan_pos_ = 0.0;
    const audio::SampleBuffer* source_buffer_ = nullptr;
    // wavetable
    const LoadedNtpPlugin::RawTable* table_ = nullptr;
    double table_phase_ = 0.0;
    // convolver — direct FIR over a ring of past inputs
    std::vector<float> ir_l_, ir_r_;
    std::vector<float> history_l_, history_r_;
    std::size_t history_pos_ = 0;

    // sampler
    struct SamplerVoice {
        bool active = false;
        const audio::SampleBuffer* buffer = nullptr;
        const ntp::SampleZone* zone = nullptr; // null for slice voices
        double pos = 0.0;
        double step = 1.0;
        double loop_start = 0.0, loop_end = 0.0; // resident frames
        double end_frame = 0.0;
        int direction = 1; // pingpong
        float gain = 1.0F;
        int note = -1;
        int choke = -1;       // interned choke group (zones + slices share one space)
        int slice_index = -1; // >= 0 when the voice plays a slice
        bool released = false;
        std::uint64_t age = 0;
    };

    std::vector<SamplerVoice> sampler_voices_;
    std::vector<const audio::SampleBuffer*> zone_buffers_; // baked; parallel to def zones
    // Per-zone user override, written by the session thread and
    // acquire-read at trigger time. Voices latch whichever buffer the
    // trigger resolved; the swap-out is fenced by the session's
    // structural republish (kSetBundle resets sampler voices in the
    // same drain, so no voice carries a retired buffer past it).
    std::vector<std::atomic<const audio::SampleBuffer*>> zone_override_;
    std::vector<int> rr_group_of_zone_; // -1 = none
    std::vector<int> rr_counters_;
    std::vector<std::uint8_t> rr_advanced_; // per-group scratch (no RT alloc)
    std::vector<int> choke_group_of_zone_;
    // Slice map (resolved at construction; empty when the node has none).
    const audio::SampleBuffer* slice_buffer_ = nullptr; // baked source
    std::atomic<const audio::SampleBuffer*> slice_override_{nullptr};
    std::vector<int> slice_note_;     // resolved trigger note per slice
    std::vector<int> slice_choke_of_; // interned with zone choke groups
    std::vector<int> slice_rr_of_;    // -1 = none
    std::vector<int> slice_rr_counters_;
    std::vector<std::uint8_t> slice_rr_advanced_;
    std::vector<std::uint8_t> slice_cut_on_off_; // gated && releaseOnGate
    std::uint64_t sampler_clock_ = 0;

    SamplerVoice* allocate_sampler_voice();
    void choke_sampler_voices(int choke_group);

    void process_granular(float* out, std::uint32_t frames, const VoiceControls* controls);
    void process_wavetable(float* out, std::uint32_t frames);
    void process_sampler(float* out, std::uint32_t frames);
    void process_envelope(float* out, std::uint32_t frames, const VoiceControls* controls);

public:
    // Sampler note interface (called by the voice layer / instance).
    void sampler_note_on(int note, float velocity);
    void sampler_note_off(int note);
    [[nodiscard]] bool sampler_active() const;

    // Deactivates every sampler voice, dropping any SampleBuffer* they
    // hold. Runs in the audio thread's kSetBundle drain — the fence
    // that lets the session retire replaced override buffers.
    void sampler_reset();

    // Session thread: points every zone (and slot-sourced slice map)
    // carrying `slot_id` at `buffer`; null reverts to the baked
    // fallback. The caller owns `buffer` and must keep it alive until
    // the reclamation fence proves the swap-out (see zone_override_).
    void set_slot_override_buffer(const std::string& slot_id, const audio::SampleBuffer* buffer);
};

} // namespace nt::plugins
