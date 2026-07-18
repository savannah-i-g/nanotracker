#include "graph/graph_model.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace nt::graph {

bool port_kinds_compatible(PortKind src, PortKind dst) {
    // Exact port of workspaceCableGraph.ts:94-104.
    if (src == PortKind::kMidi || dst == PortKind::kMidi) {
        return src == PortKind::kMidi && dst == PortKind::kMidi;
    }
    if (src == PortKind::kCv && dst != PortKind::kCv) {
        return false;
    }
    if (src == PortKind::kGate && dst != PortKind::kGate) {
        return false;
    }
    if (dst == PortKind::kAudio && (src == PortKind::kCv || src == PortKind::kGate)) {
        return false;
    }
    if (dst == PortKind::kSidechain && (src == PortKind::kCv || src == PortKind::kGate)) {
        return false;
    }
    return true;
}

const char* port_kind_name(PortKind kind) {
    switch (kind) {
    case PortKind::kAudio:
        return "audio";
    case PortKind::kSidechain:
        return "sidechain";
    case PortKind::kCv:
        return "cv";
    case PortKind::kGate:
        return "gate";
    case PortKind::kMidi:
        return "midi";
    }
    return "?";
}

const char* connect_result_message(ConnectResult result) {
    switch (result) {
    case ConnectResult::kOk:
        return "connected";
    case ConnectResult::kSourceNodeMissing:
        return "source instrument not in the workspace";
    case ConnectResult::kDestNodeMissing:
        return "destination instrument not in the workspace";
    case ConnectResult::kSourcePortMissing:
        return "source jack does not exist";
    case ConnectResult::kDestPortMissing:
        return "destination jack does not exist";
    case ConnectResult::kKindMismatch:
        return "jack kinds are incompatible";
    case ConnectResult::kDuplicateCableId:
        return "cable id already in use";
    }
    return "?";
}

Node& WorkspaceGraph::add_node(Node node) {
    nodes_.push_back(std::move(node));
    return nodes_.back();
}

bool WorkspaceGraph::remove_node(const std::string& workspace_id) {
    const int index = node_index(workspace_id);
    if (index < 0) {
        return false;
    }
    // Cables first (mirrors the web's teardown order).
    std::erase_if(cables_, [&](const Cable& c) {
        return c.source.node_id == workspace_id || c.dest.node_id == workspace_id;
    });
    nodes_.erase(nodes_.begin() + index);
    return true;
}

const Node* WorkspaceGraph::find_node(const std::string& workspace_id) const {
    const int index = node_index(workspace_id);
    return index >= 0 ? &nodes_[static_cast<std::size_t>(index)] : nullptr;
}

Node* WorkspaceGraph::find_node(const std::string& workspace_id) {
    const int index = node_index(workspace_id);
    return index >= 0 ? &nodes_[static_cast<std::size_t>(index)] : nullptr;
}

int WorkspaceGraph::node_index(const std::string& workspace_id) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].workspace_id == workspace_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ConnectResult WorkspaceGraph::connect(const CableEnd& source, const CableEnd& dest, CableMode mode,
                                      const std::string& cable_id) {
    std::string id = cable_id.empty() ? next_cable_id() : cable_id;
    if (find_cable(id) != nullptr) {
        return ConnectResult::kDuplicateCableId;
    }
    const Node* src_node = find_node(source.node_id);
    if (src_node == nullptr) {
        return ConnectResult::kSourceNodeMissing;
    }
    const Node* dst_node = find_node(dest.node_id);
    if (dst_node == nullptr) {
        return ConnectResult::kDestNodeMissing;
    }
    const Port* src_port = find_output(*src_node, source.port_id);
    if (src_port == nullptr) {
        return ConnectResult::kSourcePortMissing;
    }
    const Port* dst_port = find_input(*dst_node, dest.port_id);
    if (dst_port == nullptr) {
        return ConnectResult::kDestPortMissing;
    }
    if (!port_kinds_compatible(src_port->kind, dst_port->kind)) {
        return ConnectResult::kKindMismatch;
    }
    Cable cable;
    cable.id = std::move(id);
    cable.source = source;
    cable.dest = dest;
    cable.mode = mode;
    cable.src_kind = src_port->kind;
    cable.dst_kind = dst_port->kind;
    cables_.push_back(std::move(cable));
    return ConnectResult::kOk;
}

bool WorkspaceGraph::disconnect(const std::string& cable_id) {
    const auto before = cables_.size();
    std::erase_if(cables_, [&](const Cable& c) { return c.id == cable_id; });
    return cables_.size() != before;
}

bool WorkspaceGraph::set_cable_mode(const std::string& cable_id, CableMode mode) {
    for (Cable& cable : cables_) {
        if (cable.id == cable_id) {
            cable.mode = mode;
            return true;
        }
    }
    return false;
}

const Cable* WorkspaceGraph::find_cable(const std::string& cable_id) const {
    for (const Cable& cable : cables_) {
        if (cable.id == cable_id) {
            return &cable;
        }
    }
    return nullptr;
}

std::string WorkspaceGraph::next_node_id(const std::string& prefix) const {
    for (int n = 1;; ++n) {
        std::string candidate = prefix + "-" + std::to_string(n);
        if (find_node(candidate) == nullptr) {
            return candidate;
        }
    }
}

std::string WorkspaceGraph::next_cable_id() const {
    for (int n = 1;; ++n) {
        std::string candidate = "cbl-" + std::to_string(n);
        if (find_cable(candidate) == nullptr) {
            return candidate;
        }
    }
}

namespace {

const Port* find_in_list(const std::vector<Port>& ports, const std::string& port_id) {
    for (const Port& port : ports) {
        if (port.id == port_id) {
            return &port;
        }
    }
    return nullptr;
}

int index_in_list(const std::vector<Port>& ports, const std::string& port_id) {
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].id == port_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

const Port* find_output(const Node& node, const std::string& port_id) {
    return find_in_list(node.outputs, port_id);
}

const Port* find_input(const Node& node, const std::string& port_id) {
    return find_in_list(node.inputs, port_id);
}

int output_index(const Node& node, const std::string& port_id) {
    return index_in_list(node.outputs, port_id);
}

int input_index(const Node& node, const std::string& port_id) {
    return index_in_list(node.inputs, port_id);
}

Node make_tracker_bus_node(int channel_count) {
    Node node;
    node.workspace_id = kTrackerBusId;
    node.plugin_id = "builtin:tracker-bus";
    node.display_name = "TRACKER BUS";
    node.kind = NodeKind::kTrackerBus;
    node.window.x = 40.0F;
    node.window.y = 380.0F;
    // Web v5 layout: [audio, vol cv, midi] per channel, then the merged
    // master.midi out; master.midi.in is the bus's only input. Order
    // matters — jackIndex-era WPBR snapshots resolve positionally
    // (graph_wpbr.cpp).
    for (int ch = 0; ch < channel_count; ++ch) {
        std::array<char, 8> padded{};
        std::snprintf(padded.data(), padded.size(), "%02d", ch + 1);
        Port audio;
        audio.id = std::string("ch") + padded.data();
        audio.label = std::string("CH") + padded.data();
        audio.kind = PortKind::kAudio;
        audio.channel_ref = ch;
        node.outputs.push_back(std::move(audio));
        Port cv;
        cv.id = std::string("ch") + padded.data() + ".vol.cv";
        cv.label = std::string("VOL ") + padded.data();
        cv.kind = PortKind::kCv;
        cv.channel_ref = ch;
        node.outputs.push_back(std::move(cv));
        Port midi;
        midi.id = std::string("ch") + padded.data() + ".midi";
        midi.label = std::string("M") + padded.data();
        midi.kind = PortKind::kMidi;
        midi.channel_ref = ch;
        node.outputs.push_back(std::move(midi));
    }
    Port master_midi;
    master_midi.id = "master.midi";
    master_midi.label = "MIDI OUT";
    master_midi.kind = PortKind::kMidi;
    node.outputs.push_back(std::move(master_midi));
    Port master_midi_in;
    master_midi_in.id = "master.midi.in";
    master_midi_in.label = "MIDI IN";
    master_midi_in.kind = PortKind::kMidi;
    node.inputs.push_back(std::move(master_midi_in));
    return node;
}

Node make_master_in_node() {
    Node node;
    node.workspace_id = kMasterInId;
    node.plugin_id = "builtin:master-in";
    node.display_name = "MASTER IN";
    node.kind = NodeKind::kMasterIn;
    node.window.x = 620.0F;
    node.window.y = 380.0F;
    Port main;
    main.id = "main";
    main.label = "MAIN";
    main.kind = PortKind::kAudio;
    node.inputs.push_back(std::move(main));
    return node;
}

Node make_module_player_node() {
    Node node;
    node.workspace_id = kModulePlayerId;
    node.plugin_id = "builtin:module-player";
    node.display_name = "MODULE PLAYER";
    node.kind = NodeKind::kModulePlayer;
    node.window.x = 40.0F;
    node.window.y = 560.0F;
    Port main;
    main.id = "main";
    main.label = "OUT";
    main.kind = PortKind::kAudio;
    node.outputs.push_back(std::move(main));
    return node;
}

Node make_utility_sum_node(const std::string& workspace_id) {
    Node node;
    node.workspace_id = workspace_id;
    node.plugin_id = "builtin:sum";
    node.display_name = "SUM";
    node.kind = NodeKind::kUtilitySum;
    node.window.x = 340.0F;
    node.window.y = 460.0F;
    Port in;
    in.id = "in";
    in.label = "IN";
    in.kind = PortKind::kAudio;
    node.inputs.push_back(std::move(in));
    Port out;
    out.id = "out";
    out.label = "OUT";
    out.kind = PortKind::kAudio;
    node.outputs.push_back(std::move(out));
    return node;
}

Node make_ext_midi_in_node(const std::string& workspace_id) {
    Node node;
    node.workspace_id = workspace_id;
    node.plugin_id = "builtin:ext-midi-in";
    node.display_name = "EXT MIDI IN";
    node.kind = NodeKind::kExtMidiIn;
    node.window.x = 40.0F;
    node.window.y = 200.0F;
    Port out;
    out.id = "midi";
    out.label = "MIDI";
    out.kind = PortKind::kMidi;
    node.outputs.push_back(std::move(out));
    return node;
}

Node make_ext_midi_out_node(const std::string& workspace_id) {
    Node node;
    node.workspace_id = workspace_id;
    node.plugin_id = "builtin:ext-midi-out";
    node.display_name = "EXT MIDI OUT";
    node.kind = NodeKind::kExtMidiOut;
    node.window.x = 620.0F;
    node.window.y = 200.0F;
    Port in;
    in.id = "midi";
    in.label = "MIDI";
    in.kind = PortKind::kMidi;
    node.inputs.push_back(std::move(in));
    return node;
}

} // namespace nt::graph
