// audio/audio_engine — the real-time render core.
// Owns the device, the UI→audio command queue, and the audio→UI
// snapshot channel. Renders in fixed kBlockFrames blocks inside the
// device pull (a pull spanning a block boundary is split), which is the
// structural home the sequencer and patch graph slot into in later
// stages. Stage 2 voices: a test tone and one sample voice.
#pragma once

#include "audio/audio_device.h"
#include "audio/dsp.h"
#include "audio/fx_chain.h"
#include "audio/graph_runner.h"
#include "audio/sample_buffer.h"
#include "engine/tracker_engine.h"
#include "modplay/module_player.h"
#include "rt/spsc_queue.h"
#include "rt/triple_buffer.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace nt::audio {

inline constexpr std::uint32_t kBlockFrames = 128;

// Everything the sequencer needs to play a project, assembled off the
// audio thread and handed over by pointer (RCU style: the app owns the
// bundle and must keep it alive until a replacement bundle or stop has
// been observed in a later snapshot).
struct PlaybackBundle {
    const engine::TrackerProject* project = nullptr;
    // Built off-thread alongside the bundle; processed on the audio
    // thread. Null = no FX mixer.
    FxRack* fx_rack = nullptr;
    // Workspace patch graph evaluation plan. Null = no cables.
    GraphRunner* graph = nullptr;
    // Decoded, device-rate audio per sample id (index = id, 1-based;
    // index 0 unused). Null entries are silent.
    std::array<const SampleBuffer*, engine::kMaxSamples + 1> samples_by_id{};
    // Plugin instrument bindings per instrument-table slot (1-based;
    // null = not a plugin instrument). The sequencer routes note
    // triggers here; the instances render inside the graph runner.
    std::array<GraphPluginBinding*, engine::kMaxSamples + 1> plugin_by_slot{};
};

// Published once per device pull; read by the UI at frame rate.
struct EngineSnapshot {
    float peak_l = 0.0F;
    float peak_r = 0.0F;
    std::uint32_t sample_rate = 0;
    std::uint64_t pulls = 0;
    bool tone_active = false;
    bool voice_active = false;
    // Transport position (valid while transport_playing).
    bool transport_playing = false;
    int order_pos = 0;
    int pattern_index = 0;
    int row = 0;
    int tick = 0;
    int bpm = 0;
    int speed = 0;
    // Module Player state.
    bool module_playing = false;
    bool song_ended = false;
    // MIDI-clock PLL anchor: total frames rendered at the last publish
    // plus the host time it happened — the MIDI thread interpolates
    // between snapshots (midi/midi_out_thread.h).
    std::uint64_t stream_frames = 0;
    double host_time_seconds = 0.0;
};

// POD command set (SpscQueue requires trivially copyable). Sample
// pointers passed here must outlive playback — the registry that loads
// samples owns them and only frees at shutdown; there is no mid-run
// reclamation path.
struct Command {
    enum class Type : std::uint8_t {
        kToneOn,
        kToneOff,
        kToneFreq,
        kPlaySample,
        kStopSample,
        kSetBundle,      // bundle = new playback bundle (or null to clear)
        kSwapBundle,     // live bundle swap: transport, voices and play
                         // state survive. Only for bundles whose project
                         // pointer is unchanged (cable/graph edits).
        kTransportPlay,  // start sequencing: aux_int = order-list start
                         // position; aux_int2 = exclusive end bound
                         // (0 = none — normal looped playback; N = park
                         // the transport before order N or at song wrap,
                         // voices ring out). Export passes range end + 1.
        kTransportStop,  // stop and reset position
        kSetChannelMask, // aux_int = audible-channel bitmask (bit =
                         // channel index). Gates pattern, plugin and
                         // sequence-layer note triggers only — the
                         // sequencer still runs every channel, so
                         // global timing effects (Fxx/Bxx/Dxx) stay
                         // identical across stem passes. Reset to
                         // all-on by kSetBundle.
        kPreviewNote,    // audition: sample + value=rate ratio + aux_int=channel
        kFxParam,        // aux_int=(ch<<16)|(mod<<8)|param_idx, value=new value
        kSetModule,      // module = player to mix (null detaches)
        kModulePlay,     // start module playback
        kModuleStop,     // stop module playback
        kPluginNoteOn,   // aux_int=(slot<<8)|midi_note, value=velocity
        kPluginNoteOff,  // aux_int=(slot<<8)|midi_note
    };
    Type type = Type::kToneOff;
    float value = 0.0F;
    const SampleBuffer* sample = nullptr;
    const PlaybackBundle* bundle = nullptr;
    int aux_int = 0;
    int aux_int2 = 0;
    modplay::ModulePlayer* module = nullptr;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    // Opens the device; returns false with device error text available
    // via device().error(). Safe to call once.
    bool start();
    void stop();

    // Offline (export) mode: no device — the caller clocks the engine
    // by pulling blocks itself. Mutually exclusive with start().
    bool start_offline(std::uint32_t rate);

    void render_offline(float* interleaved, std::uint32_t frames) { render(interleaved, frames); }

    [[nodiscard]] bool running() const { return running_; }

    [[nodiscard]] const AudioDevice& device() const { return *device_; }

    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }

    // UI thread: enqueue a command (drops with a counter when the queue
    // is full — never blocks).
    void send(const Command& command);

    [[nodiscard]] std::uint64_t dropped_commands() const {
        return dropped_commands_.load(std::memory_order_relaxed);
    }

    // UI thread: most recent snapshot.
    const EngineSnapshot& snapshot() { return snapshot_.read(); }

private:
    void render(float* interleaved, std::uint32_t frames);
    void render_block(float* interleaved, std::uint32_t frames);
    void drain_commands();

    std::unique_ptr<AudioDevice> device_;
    std::uint32_t sample_rate_ = 0;
    bool running_ = false;

    rt::SpscQueue<Command> commands_{256};
    std::atomic<std::uint64_t> dropped_commands_{0};
    rt::TripleBuffer<EngineSnapshot> snapshot_;

    // Render-thread state (touched only inside render()).
    std::uint64_t pulls_ = 0;
    bool tone_on_ = false;
    float tone_freq_ = 440.0F;
    double tone_phase_ = 0.0;
    const SampleBuffer* voice_sample_ = nullptr;
    std::uint32_t voice_pos_ = 0;

    // Sequencer (audio-thread only).
    void handle_tick_outputs();
    void advance_transport_in_block(std::uint32_t frames);
    // Renders active voices into channel scratch at `offset` frames
    // into the block. Dry mix / FX / graph happen once per block in
    // render_block, after all spans have rendered.
    void render_voices_span(std::uint32_t offset, std::uint32_t frames);

    // One pitched voice per tracker channel; a retrigger replaces the
    // channel's voice (classic tracker behaviour).
    struct ChannelVoice {
        const SampleBuffer* sample = nullptr;
        double pos = 0.0;  // frames into the buffer
        double step = 1.0; // playback rate ratio
        float gain = 1.0F; // volume 0-64 → 0-1, incl. overrides
        float pan = 0.5F;  // 0..1
        // Loop points in frames of the RESIDENT buffer (converted from
        // the sample's source-rate fields at trigger — fix #9).
        double loop_start = 0.0;
        double loop_end = 0.0; // 0 = no loop
        bool active = false;
    };

    const PlaybackBundle* bundle_ = nullptr;
    engine::TrackerPlayState play_state_{};
    bool transport_playing_ = false;
    double frames_until_tick_ = 0.0;
    // Exclusive order-list bound from kTransportPlay (0 = none). When
    // set, reaching it (or song wrap) parks the transport instead of
    // looping — the offline export's range end.
    int transport_end_bound_ = 0;
    // kSetChannelMask trigger gate (stems export); all-on by default.
    std::uint32_t channel_mask_ = 0xFFFFFFFFU;
    std::array<ChannelVoice, engine::kMaxChannels> voices_{};
    // Last plugin note per channel so release/retrigger can note_off.
    std::array<int, engine::kMaxChannels> plugin_note_{};
    std::array<int, engine::kMaxChannels> plugin_slot_{};

    // Sequence-layer voices (piano roll): layered on top of channel
    // voices, sample instruments only — plugin instruments route
    // through their bindings. Fixed pool, oldest-steal.
    struct SeqVoice {
        const SampleBuffer* sample = nullptr;
        double pos = 0.0;
        double step = 1.0;
        float gain = 1.0F;
        int pitch = -1;
        int channel = -1;
        int layer = -1;
        std::uint64_t age = 0;
        bool active = false;
    };

    static constexpr int kMaxSeqVoices = 32;
    std::array<SeqVoice, kMaxSeqVoices> seq_voices_{};
    std::uint64_t seq_voice_clock_ = 0;
    std::array<float, static_cast<std::size_t>(kBlockFrames) * 2> seq_scratch_{};
    void handle_seq_triggers();
    void render_seq_voices_span(std::uint32_t offset, std::uint32_t frames);
    std::uint64_t stream_frames_ = 0;

    // Module Player source (owned by the app; attached via command).
    modplay::ModulePlayer* module_ = nullptr;
    bool module_playing_ = false;

    // Mixer: per-tracker-channel stereo scratch feeding dry mix, FX
    // sends and the patch graph's tracker-bus outs; master dynamics +
    // DC blocking at the end of every block.
    std::array<std::array<float, static_cast<std::size_t>(kBlockFrames) * 2>, engine::kMaxChannels>
        channel_scratch_{};
    // Module player block (graph source + direct mix) and per-channel
    // gains for the bus volume-CV outs.
    std::array<float, static_cast<std::size_t>(kBlockFrames) * 2> module_scratch_{};
    std::array<float, engine::kMaxChannels> channel_gains_{};
    dsp::Compressor master_compressor_;
    dsp::DcBlocker master_dc_l_;
    dsp::DcBlocker master_dc_r_;
    bool master_configured_ = false;
};

} // namespace nt::audio
