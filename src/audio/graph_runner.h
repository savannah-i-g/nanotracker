// audio/graph_runner — block evaluation of the workspace patch graph
// on the audio thread. Built off-thread from the model + compiled
// schedule (all buffers preallocated); process() is allocation-free.
//
// Signal semantics per block:
//   audio/sidechain — interleaved stereo float blocks, summed at
//                     input ports (fan-in) and fanned out at outputs.
//   cv              — mono float blocks. An audio source driving a cv
//                     input is downmixed ((L+R)/2), matching the web's
//                     "host-side scaling" allowance.
//   gate            — per-block edge-event lists with exact frame
//                     offsets, derived from the source signal with a
//                     0.5 threshold + hysteresis. This replaces the
//                     web's ~60Hz analyser polling
//                     (workspaceCableGraph.ts:107-146, fix #11).
//   midi            — per-block frame-stamped message lists
//                     (audio/midi_event.h) written by source ports and
//                     merged at input ports: concatenate inbound lists
//                     in schedule-edge order, stable-sort by frame.
//                     Double-buffered like audio, so delayed midi
//                     edges read the previous block (bounded feedback
//                     instead of the web's hop counter).
//
// Edges marked delayed by the compiler read the source port's
// previous-block buffer — the one-block feedback delay that makes
// cycles legal (fix #16).
#pragma once

#include "audio/midi_event.h"
#include "graph/graph_compile.h"
#include "graph/graph_model.h"
#include "rt/spsc_queue.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nt::audio {

inline constexpr std::uint32_t kGraphBlockFrames = 128;

// Engine-side signals the built-in nodes bind to, set per block.
struct GraphBlockContext {
    // Per tracker channel, interleaved stereo, frames*2 valid floats.
    const float* const* channel_scratch = nullptr;
    int channel_count = 0;
    // Per tracker channel, current linear gain (volume CV source).
    const float* channel_gains = nullptr;
    // Module player block (interleaved stereo) or null when silent.
    const float* module_block = nullptr;
    // Master bus accumulator (interleaved stereo); Master In adds here.
    float* master_accum = nullptr;
    // Per tracker channel, this block's row-derived midi events
    // (frame-stamped at tick offsets by the sequencer). Null array or
    // entry = silent. The tracker bus copies these onto chNN.midi and
    // merges them all onto master.midi.
    const MidiEventList* const* channel_midi = nullptr;
    // This block's hardware MIDI arrivals (already frame-stamped by
    // the engine's ring drain); every Ext MIDI In node mirrors it.
    // Null = no device open / no arrivals.
    const MidiEventList* ext_midi_in = nullptr;
    // Ext MIDI Out sink: encoded wire bytes with absolute stream-
    // sample timestamps. Audio-thread producer; the MIDI out thread is
    // the single consumer. Null = discard (no hardware out).
    rt::SpscQueue<ExtMidiOutMessage>* ext_midi_out = nullptr;
    // master.midi.in sink — the record/step-entry hook. Cabled events
    // reaching the tracker bus's input land here; the UI half drains
    // via AudioEngine::poll_bus_midi_in. Null = discard.
    rt::SpscQueue<TimedMidiMessage>* bus_midi_in = nullptr;
    // Absolute stream frame of this block's first sample (timestamp
    // base for the two sinks above).
    std::uint64_t block_start_frame = 0;
    // Device rate, for the CV→param slew coefficient.
    std::uint32_t sample_rate = 48000;
};

// Implemented by hosted plugin instances (NTP now; external CLAP/VST3
// plugins with their stage). The runner calls process_block for
// kPlugin nodes; the engine forwards sequencer notes through the same
// interface. All methods run on the audio thread and must not allocate.
class GraphPluginBinding {
public:
    GraphPluginBinding() = default;
    GraphPluginBinding(const GraphPluginBinding&) = default;
    GraphPluginBinding& operator=(const GraphPluginBinding&) = default;
    GraphPluginBinding(GraphPluginBinding&&) = default;
    GraphPluginBinding& operator=(GraphPluginBinding&&) = default;
    virtual ~GraphPluginBinding() = default;

    virtual void process_block(const float* in, float* out, std::uint32_t frames) = 0;

    virtual void plugin_note_on(int note, float velocity) = 0;
    virtual void plugin_note_off(int note) = 0;

    // CV→host-param delivery (audio thread, allocation-free).
    // `param_index` is the position in this binding's parameter list —
    // the same index the session used when generating the node's CV
    // ports (graph::Port::param_ref); `value01` is the slewed block
    // value, already clamped to 0..1. Each binding maps it into its
    // own parameter domain. Default: no parameter surface.
    virtual void plugin_set_param_cv(int /*param_index*/, float /*value01*/) {}
};

struct GateEvent {
    std::uint32_t frame = 0;
    bool on = false;
};

inline constexpr int kMaxGateEventsPerBlock = 32;

struct GateEventList {
    std::array<GateEvent, kMaxGateEventsPerBlock> events{};
    int count = 0;
};

class GraphRunner {
public:
    GraphRunner(const graph::WorkspaceGraph& model, graph::GraphSchedule schedule);

    // Audio thread. frames <= kGraphBlockFrames.
    void process(const GraphBlockContext& ctx, std::uint32_t frames);

    [[nodiscard]] std::uint32_t suppressed_channel_mask() const {
        return schedule_.suppressed_channel_mask;
    }

    [[nodiscard]] bool module_suppressed() const { return schedule_.module_suppressed; }

    // Binds a plugin instance to a kPlugin node (model index). Called
    // at build time, before the runner reaches the audio thread.
    void bind_plugin(int node_index, GraphPluginBinding* binding);

    // Test access: the current-block buffer of an output port (stereo
    // frames*2 for audio, frames for cv), the summed input of an audio
    // input port, and the gate/midi events of typed input ports (plus
    // the just-written midi output list). Node/port are model indices.
    [[nodiscard]] const float* debug_output(int node, int port) const;
    [[nodiscard]] const float* debug_audio_input(int node, int port) const;
    [[nodiscard]] const GateEventList& debug_gate_input(int node, int port) const;
    [[nodiscard]] const MidiEventList& debug_midi_input(int node, int port) const;
    [[nodiscard]] const MidiEventList& debug_midi_output(int node, int port) const;

private:
    struct PortSlot {
        graph::PortKind kind = graph::PortKind::kAudio;
        int channel_ref = -1;
        // Offsets into pool_ for the double-buffered output (cur/prev)
        // or the input sum buffer; -1 = no buffer (midi, gate outs).
        int buf_a = -1;
        int buf_b = -1;
        int gate_list = -1; // index into gate_lists_ for gate inputs
        // Midi ports: indices into midi_lists_ — outputs use a/b as
        // the cur/prev pair (flip_-selected), inputs the merge list.
        int midi_a = -1;
        int midi_b = -1;
        // CV param ports on plugin nodes (graph::Port::param_ref).
        int param_ref = -1;
        // True when at least one schedule edge feeds this input — CV
        // param delivery only runs on actually-cabled ports (an
        // uncabled port must not drag its param to zero).
        bool driven = false;
        // CV→param slew state (single-pole toward the block average).
        float cv_state = 0.0F;
    };

    struct NodeSlot {
        graph::NodeKind kind = graph::NodeKind::kUtilitySum;
        std::vector<PortSlot> inputs;
        std::vector<PortSlot> outputs;
        int first_edge = 0; // range into schedule_.edges (sorted by dst)
        int edge_count = 0;
        // Plugin nodes: instance + master-route strip (volume/pan/
        // bypass from the workspace model; suppressed = reroute cable).
        GraphPluginBinding* plugin = nullptr;
        float volume = 1.0F;
        float pan = 0.0F;
        bool bypass = false;
        bool master_suppressed = false;
    };

    [[nodiscard]] float* out_cur(const PortSlot& slot);
    [[nodiscard]] const float* out_prev(const PortSlot& slot) const;
    [[nodiscard]] MidiEventList& midi_out_cur(const PortSlot& slot);
    [[nodiscard]] const MidiEventList& midi_out_prev(const PortSlot& slot) const;

    void gather_inputs(const NodeSlot& node, std::uint32_t frames);
    void run_node(NodeSlot& node, const GraphBlockContext& ctx, std::uint32_t frames);

    graph::GraphSchedule schedule_;
    std::vector<NodeSlot> nodes_;
    std::vector<float> pool_;
    std::vector<GateEventList> gate_lists_;
    std::vector<MidiEventList> midi_lists_;
    // Per-edge gate hysteresis state (audio→gate edges), persistent
    // across blocks.
    std::vector<bool> gate_high_;
    bool flip_ = false; // selects cur/prev halves of output buffers
};

} // namespace nt::audio
