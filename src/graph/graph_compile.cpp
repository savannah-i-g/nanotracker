#include "graph/graph_compile.h"

#include <algorithm>

namespace nt::graph {

namespace {

// Iterative DFS colouring for back-edge detection. White = unvisited,
// grey = on the current DFS path, black = finished. An edge into a
// grey node closes a cycle and becomes the delayed edge.
enum class Colour : std::uint8_t { kWhite, kGrey, kBlack };

void dfs_mark_cycles(const std::vector<std::vector<int>>& adjacency,
                     std::vector<ScheduleEdge>& edges, std::vector<Colour>& colour,
                     std::vector<int>& postorder, int start) {
    if (colour[static_cast<std::size_t>(start)] != Colour::kWhite) {
        return;
    }
    // Explicit stack of (node, next-neighbour cursor).
    std::vector<std::pair<int, std::size_t>> stack;
    colour[static_cast<std::size_t>(start)] = Colour::kGrey;
    stack.emplace_back(start, 0);
    while (!stack.empty()) {
        auto& [node, cursor] = stack.back();
        const std::vector<int>& out = adjacency[static_cast<std::size_t>(node)];
        if (cursor >= out.size()) {
            colour[static_cast<std::size_t>(node)] = Colour::kBlack;
            postorder.push_back(node);
            stack.pop_back();
            continue;
        }
        ScheduleEdge& edge = edges[static_cast<std::size_t>(out[cursor])];
        ++cursor;
        const int next = edge.dst_node;
        switch (colour[static_cast<std::size_t>(next)]) {
        case Colour::kWhite:
            colour[static_cast<std::size_t>(next)] = Colour::kGrey;
            stack.emplace_back(next, 0);
            break;
        case Colour::kGrey:
            edge.delayed = true; // closes a cycle (includes self-loops)
            break;
        case Colour::kBlack:
            break; // forward/cross edge — fine
        }
    }
}

} // namespace

GraphSchedule compile_graph(const WorkspaceGraph& graph) {
    GraphSchedule schedule;
    const std::vector<Node>& nodes = graph.nodes();

    for (const Cable& cable : graph.cables()) {
        const int src_node = graph.node_index(cable.source.node_id);
        const int dst_node = graph.node_index(cable.dest.node_id);
        if (src_node < 0 || dst_node < 0) {
            continue;
        }
        const Node& src = nodes[static_cast<std::size_t>(src_node)];
        const Node& dst = nodes[static_cast<std::size_t>(dst_node)];
        const int src_port = output_index(src, cable.source.port_id);
        const int dst_port = input_index(dst, cable.dest.port_id);
        if (src_port < 0 || dst_port < 0) {
            continue;
        }
        const Port& out = src.outputs[static_cast<std::size_t>(src_port)];

        // Reroute suppression is per source jack (see header).
        if (cable.mode == CableMode::kReroute && out.kind == PortKind::kAudio) {
            if (src.kind == NodeKind::kTrackerBus && out.channel_ref >= 0 && out.channel_ref < 32) {
                schedule.suppressed_channel_mask |= 1U << static_cast<unsigned>(out.channel_ref);
            } else if (src.kind == NodeKind::kModulePlayer) {
                schedule.module_suppressed = true;
            } else if (src.kind == NodeKind::kPlugin) {
                schedule.suppressed_plugin_nodes.push_back(src_node);
            }
        }

        ScheduleEdge edge;
        edge.src_node = src_node;
        edge.src_port = src_port;
        edge.dst_node = dst_node;
        edge.dst_port = dst_port;
        edge.src_kind = out.kind;
        edge.dst_kind = dst.inputs[static_cast<std::size_t>(dst_port)].kind;
        schedule.edges.push_back(edge);
    }

    // Adjacency in declaration order keeps the result deterministic.
    std::vector<std::vector<int>> adjacency(nodes.size());
    for (std::size_t i = 0; i < schedule.edges.size(); ++i) {
        adjacency[static_cast<std::size_t>(schedule.edges[i].src_node)].push_back(
            static_cast<int>(i));
    }

    std::vector<Colour> colour(nodes.size(), Colour::kWhite);
    std::vector<int> postorder;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        dfs_mark_cycles(adjacency, schedule.edges, colour, postorder, static_cast<int>(i));
    }

    // Reverse postorder of the DFS with delayed edges ignored is a
    // topological order of the remaining DAG.
    schedule.order.assign(postorder.rbegin(), postorder.rend());

    // Group edges by destination for the runner's input gathering.
    std::stable_sort(
        schedule.edges.begin(), schedule.edges.end(),
        [](const ScheduleEdge& a, const ScheduleEdge& b) { return a.dst_node < b.dst_node; });
    return schedule;
}

} // namespace nt::graph
