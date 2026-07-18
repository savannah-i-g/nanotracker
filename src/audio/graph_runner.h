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
//
// Edges marked delayed by the compiler read the source port's
// previous-block buffer — the one-block feedback delay that makes
// cycles legal (fix #16).
#pragma once

#include "graph/graph_compile.h"
#include "graph/graph_model.h"

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
    // input port, and the gate events of a gate input port. Node/port
    // are model indices.
    [[nodiscard]] const float* debug_output(int node, int port) const;
    [[nodiscard]] const float* debug_audio_input(int node, int port) const;
    [[nodiscard]] const GateEventList& debug_gate_input(int node, int port) const;

private:
    struct PortSlot {
        graph::PortKind kind = graph::PortKind::kAudio;
        int channel_ref = -1;
        // Offsets into pool_ for the double-buffered output (cur/prev)
        // or the input sum buffer; -1 = no buffer (midi, gate outs).
        int buf_a = -1;
        int buf_b = -1;
        int gate_list = -1; // index into gate_lists_ for gate inputs
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

    void gather_inputs(const NodeSlot& node, std::uint32_t frames);
    void run_node(const NodeSlot& node, const GraphBlockContext& ctx, std::uint32_t frames);

    graph::GraphSchedule schedule_;
    std::vector<NodeSlot> nodes_;
    std::vector<float> pool_;
    std::vector<GateEventList> gate_lists_;
    // Per-edge gate hysteresis state (audio→gate edges), persistent
    // across blocks.
    std::vector<bool> gate_high_;
    bool flip_ = false; // selects cur/prev halves of output buffers
};

} // namespace nt::audio
