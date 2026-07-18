// engine/tracker_types — the tracker data model.
// Semantics mirror the web engine's model (behavioural reference:
// Source/.../src/lib/trackerEngine.ts, trackerSeqEngine.ts,
// trackerFxEngine.ts); representation is native. Projects are built and
// edited off the audio thread; during playback the audio thread reads
// them through an immutable snapshot pointer, so every structure here
// is read-only in the render path.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nt::engine {

inline constexpr int kMaxChannels = 32;
inline constexpr int kMaxSamples = 31;
inline constexpr int kNoteOff = 97; // note value meaning "release"
inline constexpr int kMaxNote = 96; // C-0..B-7 → 1..96
inline constexpr int kMaxSeqLayersPerChannel = 4;

// ── Pattern data ─────────────────────────────────────────────────────

struct TrackerCell {
    std::uint8_t note = 0;       // 0=empty, 1-96, 97=note-off
    std::uint8_t instrument = 0; // 0=empty, 1-31
    std::uint8_t volume = 0xFF;  // 0xFF=default, else 0x00-0x40
    std::uint8_t effect = 0;     // 0x0-0xF
    std::uint8_t effect_param = 0;
    std::uint8_t bound_index = 0; // bound-instrument sub-slot (see engine)
};

struct TrackerPattern {
    int id = 0;
    std::string name;
    // rows[row][channel]
    std::vector<std::vector<TrackerCell>> rows;
    int highlight_major = 0; // 0 = inherit project default
    int highlight_minor = 0;
};

// ── Samples & instruments ────────────────────────────────────────────

// Sample metadata as stored in projects. The decoded, device-rate audio
// lives in audio/sample_buffer; loop points here keep the file's
// source-rate frame semantics for FTRK compatibility and are converted
// at load (fix-don't-retain #9).
struct TrackerSample {
    int id = 0; // 1-31
    std::string name;
    std::string file_name;
    std::string format;                      // "wav" | "ogg" | "mp3"
    std::vector<std::uint8_t> original_data; // original encoded file bytes
    std::uint32_t sample_rate = 0;
    std::uint32_t num_channels = 0;
    std::uint32_t frames = 0;
    std::uint32_t loop_start = 0;
    std::uint32_t loop_length = 0;
    int base_note = 60; // MIDI
    int finetune = 0;   // -128..+127 cents
    int volume = 64;    // 0-64
    int pan = 128;      // 0-255
    double stretch_ratio = 1.0;
    int category = 0;
};

enum class InstrumentSourceType : std::uint8_t { kSample, kPlugin, kWorkspace };

struct InstrumentTableEntry {
    InstrumentSourceType type = InstrumentSourceType::kSample;
    int sample_id = 0;
    std::string plugin_id;
    std::vector<std::pair<std::string, double>> plugin_preset_params;
    std::vector<int> bound_tracks; // zero-indexed channels, sorted+deduped
    std::string workspace_id;
};

// ── Sequence layer (piano roll) ──────────────────────────────────────

struct SequenceNote {
    int pitch = 60;         // MIDI 0-127
    int start_tick = 0;     // row * speed + sub-tick
    int duration_ticks = 1; // >= 1
    int velocity = 100;     // 0-127
};

struct SequenceLayer {
    int instrument = 0;              // instrument table slot, 1-based; 0 = silent
    std::vector<SequenceNote> notes; // sorted by start_tick
    bool enabled = true;
};

struct SequencePattern {
    // layers[channel][layer]
    std::vector<std::vector<SequenceLayer>> layers;
};

struct SequenceMixerState {
    std::vector<SequencePattern> seq_patterns; // parallel to patterns
};

// ── FX mixer (automation surface only, engine-side) ─────────────────

struct FxCell {
    int channel_index = 0;
    int module_index = 0;
    std::string param_key;
    double value = 0.0;
    std::string pedal_workspace_id; // empty = mixer target
    std::string pedal_param_key;
};

struct FxPattern {
    // One optional automation cell per row; index parallel to rows.
    std::vector<bool> present;
    std::vector<FxCell> cells; // valid where present[row]
};

struct FxModuleInstance {
    std::string module_id; // registry id ("delay", "reverb", ...)
    bool enabled = true;
    std::vector<std::pair<std::string, float>> params;
};

struct FxChannelStrip {
    std::string name;
    int volume = 100; // 0-100
    int pan = 0;      // -100..100
    std::uint32_t color = 0;
    bool enabled = true;
    std::vector<FxModuleInstance> modules;
    std::vector<float> tracker_sends; // per tracker channel, 0..1
};

struct FxMixerState {
    std::vector<FxChannelStrip> channels;
    std::vector<FxPattern> fx_patterns; // parallel to patterns
};

// ── Project ──────────────────────────────────────────────────────────

enum class FreqTable : std::uint8_t { kAmiga, kLinear };

struct TrackerProject {
    std::string name = "UNTITLED";
    int bpm = 125;
    int speed = 6; // ticks per row, 1-31
    int rows_per_pattern = 64;
    int channels = 4;
    int highlight_major = 16;
    int highlight_minor = 4;
    std::vector<int> order_list; // pattern ids in play order
    std::vector<TrackerPattern> patterns;
    std::vector<TrackerSample> samples;
    FreqTable freq_table = FreqTable::kAmiga;
    std::vector<InstrumentTableEntry> instrument_table; // empty = legacy direct-sample mapping
    SequenceMixerState sequence_mixer;
    FxMixerState fx_mixer;
    std::array<std::uint32_t, kMaxChannels> channel_colors{}; // 0 = default palette
};

// ── Play state ───────────────────────────────────────────────────────

struct ChannelPlayState {
    int note = 0;
    int instrument = 0;
    int volume = 64;
    int pan = 128;
    int period = 0;
    // Effect memory
    int porta_target = 0;
    int porta_speed = 0;
    int vibrato_phase = 0;
    int vibrato_speed = 0;
    int vibrato_depth = 0;
    int tremolo_phase = 0;
    int tremolo_speed = 0;
    int tremolo_depth = 0;
    int sample_offset = 0;
    int retrig_count = 0;
    int retrig_param = 0;
    int arpeggio0 = 0;
    int arpeggio1 = 0;
    int arpeggio2 = 0;
    int vol_slide = 0;
    int finetune = 0;
    // Per-tick signals to the audio driver
    bool trigger_note = false;
    bool release_note = false;
    int period_override = 0;  // 0 = use period
    int volume_override = -1; // -1 = use volume
    // Raw effect bytes of the most recent row trigger (webview
    // forwarding surface in the web app; kept for parity and the
    // effect→MIDI translator).
    int row_effect = 0;
    int row_effect_param = 0;
    // Instrument routing for the audio driver
    InstrumentSourceType instrument_type = InstrumentSourceType::kSample;
    // plugin/workspace ids are resolved by the driver via the
    // instrument table entry (index = instrument-1); the engine does
    // not copy strings on the render path.
};

struct SequenceTrigger {
    int channel_index = 0;
    int layer_index = 0;
    int pitch = 0;
    int velocity = 0;
    bool is_note_on = false;
    int instrument = 0;
};

inline constexpr int kMaxSeqTriggersPerTick = 64;

struct TrackerPlayState {
    bool is_playing = false;
    bool is_paused = false;
    int order_pos = 0;
    int pattern_index = 0;
    int row = 0;
    int tick = 0;
    int bpm = 125;
    int speed = 6;
    std::array<ChannelPlayState, kMaxChannels> channels{};
    bool pattern_ended = false;
    bool song_ended = false;
    // Pattern loop (E6x)
    int loop_row = 0;
    int loop_count = 0;
    bool loop_active = false;
    bool has_loop_jump = false;      // one-shot within a single advance_tick
    bool pattern_loop_flush = false; // one-shot: release sustained voices on loop rewind
    // Pattern delay (EEx)
    int pattern_delay = 0;
    // Deferred position jump (Bxx/Dxx)
    int next_order_pos = 0;
    int next_row = 0;
    bool has_jump = false;
    // FX automation cell fired this tick (tick 0 only), else nullptr.
    const FxCell* fx_automation_cell = nullptr;
    // Sequence triggers for this tick (fixed capacity — RT path).
    std::array<SequenceTrigger, kMaxSeqTriggersPerTick> seq_triggers{};
    int seq_trigger_count = 0;
    std::uint32_t seq_triggers_dropped = 0;
};

} // namespace nt::engine
