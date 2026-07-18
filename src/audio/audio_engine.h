// audio/audio_engine — the real-time render core.
// Owns the device, the UI→audio command queue, and the audio→UI
// snapshot channel. Renders in fixed kBlockFrames blocks inside the
// device pull (a pull spanning a block boundary is split), which is the
// structural home the sequencer and patch graph slot into in later
// stages. Stage 2 voices: a test tone and one sample voice.
#pragma once

#include "audio/audio_device.h"
#include "audio/dsp.h"
#include "audio/effect_midi.h"
#include "audio/fx_chain.h"
#include "audio/graph_runner.h"
#include "audio/midi_event.h"
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
    // Serial of the last kSetBundle/kSwapBundle applied, recorded at
    // the command drain and published at end of pull — the reclamation
    // fence's proof of progress (audio/sample_reclaim.h).
    std::uint64_t bundle_serial = 0;
    // MIDI-clock PLL anchor: total frames rendered at the last publish
    // plus the host time it happened — the MIDI thread interpolates
    // between snapshots (midi/midi_out_thread.h).
    std::uint64_t stream_frames = 0;
    double host_time_seconds = 0.0;
    // Frames elapsed since the current tick's edge, at publish time
    // (0 = the publish landed exactly on the tick boundary). The record
    // quantiser (app/midi_record.h) subtracts this from stream_frames to
    // recover the frame the (row, tick) above actually began — without
    // it the anchor reads up to one tick early near row boundaries.
    // Valid while transport_playing.
    std::uint32_t tick_frame_remainder = 0;
};

// POD command set (SpscQueue requires trivially copyable). Sample and
// bundle pointers passed here must stay alive until the reclamation
// fence proves the audio thread is past them — the session's
// SampleReclaimer owns that discipline (audio/sample_reclaim.h).
struct Command {
    enum class Type : std::uint8_t {
        kToneOn,
        kToneOff,
        kToneFreq,
        kPlaySample,     // one-shot preview voice = sample. serial (when
                         // non-zero) fences the buffer it replaces:
                         // ProjectSession::preview_file retires the old
                         // preview buffer at this serial and the engine
                         // echoes it into bundle_serial (sample_reclaim.h).
        kStopSample,     // clears the one-shot preview voice; serial
                         // fences the retired preview buffer as above.
        kSetBundle,      // bundle = new playback bundle (or null to
                         // clear). Deactivates every voice class that
                         // holds a SampleBuffer* (channel, sequence-
                         // layer, one-shot preview) — the reclamation
                         // fence counts on no voice surviving a full
                         // bundle replacement.
        kSwapBundle,     // live bundle swap: transport, voices and play
                         // state survive. Only for bundles whose
                         // project pointer AND sample set are unchanged
                         // (cable/graph edits) — surviving voices keep
                         // raw SampleBuffer*s across the swap, so every
                         // buffer they can hold must still be
                         // referenced by the new bundle.
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
        kSetTempo,       // aux_int=bpm, aux_int2=speed (0 = leave that
                         // field): live transport tempo — the next tick
                         // recomputes its interval, so it is audible at
                         // once. The project holds the persisted set
                         // point; kTransportPlay re-seeds from it.
    };
    Type type = Type::kToneOff;
    float value = 0.0F;
    const SampleBuffer* sample = nullptr;
    const PlaybackBundle* bundle = nullptr;
    int aux_int = 0;
    int aux_int2 = 0;
    modplay::ModulePlayer* module = nullptr;
    // kSetBundle/kSwapBundle publish serial for the reclamation fence
    // (0 = unfenced sender, e.g. the demo path — never rewinds the
    // engine's running maximum).
    std::uint64_t serial = 0;
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

    // ── MIDI graph endpoints ─────────────────────────────────────────
    // Wires the hardware input's graph ring (midi::MidiInput) into the
    // render path; the engine drains it once per block for Ext MIDI In
    // nodes. Main thread; the pointer must outlive the engine or be
    // cleared with null first.
    void set_ext_midi_source(rt::SpscQueue<MidiMessage>* ring) {
        ext_midi_source_.store(ring, std::memory_order_release);
    }

    // Ext MIDI Out deliveries (graph → hardware). Single consumer: the
    // MIDI out thread, which dispatches by at_sample against its PLL
    // clock. Without that thread (offline/export) the ring simply
    // fills and drops — bounded either way.
    bool poll_ext_midi_out(ExtMidiOutMessage& out) { return ext_midi_out_.pop(out); }

    // master.midi.in deliveries — the pattern-record/step-entry hook.
    // Single consumer: the UI half drains once per frame and quantises
    // at_frame against the transport snapshot.
    bool poll_bus_midi_in(TimedMidiMessage& out) { return bus_midi_in_.pop(out); }

private:
    void render(float* interleaved, std::uint32_t frames);
    void render_block(float* interleaved, std::uint32_t frames, std::uint64_t block_start);
    void drain_commands();

    std::unique_ptr<AudioDevice> device_;
    std::uint32_t sample_rate_ = 0;
    bool running_ = false;

    rt::SpscQueue<Command> commands_{256};
    std::atomic<std::uint64_t> dropped_commands_{0};
    rt::TripleBuffer<EngineSnapshot> snapshot_;

    // Render-thread state (touched only inside render()).
    std::uint64_t pulls_ = 0;
    std::uint64_t bundle_serial_ = 0;
    bool tone_on_ = false;
    float tone_freq_ = 440.0F;
    double tone_phase_ = 0.0;
    const SampleBuffer* voice_sample_ = nullptr;
    std::uint32_t voice_pos_ = 0;

    // Sequencer (audio-thread only). `frame_offset` is the tick's
    // frame position inside the current block — the stamp for the
    // channel midi events this tick emits.
    void handle_tick_outputs(std::uint32_t frame_offset);
    // `block_start` is this block's absolute stream frame, so each tick
    // edge is stamped in stream-frame terms (tick_edge_frames_).
    void advance_transport_in_block(std::uint32_t frames, std::uint64_t block_start);
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
    // Absolute stream frame of the current tick's edge (updated each
    // time a tick fires). stream_frames_ minus this is the sub-tick
    // remainder the snapshot publishes for the record quantiser.
    std::uint64_t tick_edge_frames_ = 0;
    // ── Tracker-bus MIDI production ──────────────────────────────────
    // Per-channel row events for the current block (note on/off + the
    // effect→MIDI translation), frame-stamped at tick offsets; the
    // graph's tracker bus copies them onto chNN.midi / master.midi.
    std::array<MidiEventList, engine::kMaxChannels> channel_midi_{};
    // Effect→MIDI carrier state and the held note per channel (the
    // pattern lane is monophonic, so retrigger/release pair by
    // channel). Reset on kSetBundle, like the web state's bus lifetime.
    std::array<EffectMidiState, engine::kMaxChannels> midi_fx_state_{};
    std::array<int, engine::kMaxChannels> midi_note_{};
    // Hardware MIDI bridge rings (see the public poll_* methods).
    std::atomic<rt::SpscQueue<MidiMessage>*> ext_midi_source_{nullptr};
    MidiEventList ext_midi_in_events_{};
    rt::SpscQueue<ExtMidiOutMessage> ext_midi_out_{512};
    rt::SpscQueue<TimedMidiMessage> bus_midi_in_{256};
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
