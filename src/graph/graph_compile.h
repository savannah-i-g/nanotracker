// graph/graph_compile — turns the workspace graph model into a flat,
// index-based evaluation schedule for the audio thread.
//
// Cycles are legal: any edge that closes a cycle is marked `delayed`
// and reads its source's previous-block output (one-block feedback
// delay, standard modular-synth semantics — fix-don't-retain #16; the
// web banned self-feedback outright, CableOverlay.tsx:319). Removing
// the delayed edges leaves a DAG; `order` is a topological order of
// that DAG, so every non-delayed edge reads a buffer written earlier
// in the same block.
//
// Reroute suppression is resolved here, per source jack: a reroute
// cable from a tracker-bus channel out suppresses that channel's dry
// path into the master mix, and one from the module player suppresses
// its direct mix. (The web suppressed per instrument, which made
// reroute on tracker-bus cables a silent no-op — see FIXES.md.)
#pragma once

#include "graph/graph_model.h"

#include <cstdint>
#include <vector>

namespace nt::graph {

struct ScheduleEdge {
    int src_node = 0; // indices into WorkspaceGraph::nodes()
    int src_port = 0; // index into the source node's outputs
    int dst_node = 0;
    int dst_port = 0; // index into the dest node's inputs
    PortKind src_kind = PortKind::kAudio;
    PortKind dst_kind = PortKind::kAudio;
    bool delayed = false;
};

struct GraphSchedule {
    // Node evaluation order (indices into the model's node list).
    std::vector<int> order;
    // All evaluated edges, sorted by dst_node so the runner can gather
    // a node's inputs from one contiguous range. MIDI cables are not
    // scheduled (message transport lands with the MIDI stage).
    std::vector<ScheduleEdge> edges;
    // Bit N set = tracker channel N has a reroute cable from its bus
    // jack; the engine skips its dry sum into the master mix.
    std::uint32_t suppressed_channel_mask = 0;
    // Same for the module player's direct mix.
    bool module_suppressed = false;
    // Plugin nodes (model indices) whose default master route is
    // suppressed by a reroute cable from an audio out.
    std::vector<int> suppressed_plugin_nodes;
};

// Pure function of the model; deterministic (iteration in node/cable
// declaration order). Cables with unresolvable endpoints are skipped —
// WorkspaceGraph::connect validates, so those only exist transiently.
[[nodiscard]] GraphSchedule compile_graph(const WorkspaceGraph& graph);

} // namespace nt::graph
