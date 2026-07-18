// graph/graph_model — the workspace patch graph: nodes, typed ports,
// cables, and connection validation. Behavioural reference:
// Source/.../src/lib/workspaceCableGraph.ts and workspaceTrackerBus.ts /
// workspaceMasterIn.ts; representation is native.
//
// Every port has a stable string ID; indices are a render detail only
// (fix-don't-retain #14/#15 — the web kept a legacy positional model in
// parallel and committed drags by index). Connection failures return a
// typed result the UI turns into visible feedback (fix #13 — the web
// only console.warn'ed).
//
// The model is UI-thread data. The audio thread never sees it: the
// schedule compiler (graph_compile.h) and runner (audio/graph_runner.h)
// derive an immutable evaluation plan that is handed over RCU-style.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt::graph {

// Jack kinds, ported 1:1 from the web's PluginPortKind.
enum class PortKind : std::uint8_t { kAudio, kSidechain, kCv, kGate, kMidi };

// Cable modes (workspaceCableGraph.ts:63):
//   tap     — the cable is in addition to the source's default route.
//   reroute — the source's default route is suppressed; audio flows
//             only through cables.
enum class CableMode : std::uint8_t { kTap, kReroute };

// Node kinds the runner knows how to evaluate. kPlugin covers NTP and
// external plugins once their stages land; until then plugin entries
// from loaded projects stay dormant (graph_wpbr.h).
enum class NodeKind : std::uint8_t {
    kTrackerBus,   // per-channel audio/CV/MIDI outs fed by the sequencer
    kMasterIn,     // audio sink summing into the master bus
    kModulePlayer, // the libopenmpt player's stereo out
    kUtilitySum,   // pass-through summing node (audio in → audio out)
    kPlugin,       // NTP / external plugin instance
    kExtMidiIn,    // hardware MIDI input bridged onto a midi out jack
    kExtMidiOut,   // midi in jack draining to the hardware MIDI output
};

// Kind-compatibility matrix, ported exactly from
// workspaceCableGraph.ts:94-104: midi only pairs with midi; cv and
// gate sources only drive their own kind; audio/sidechain destinations
// reject cv and gate; everything else (including audio → cv and
// audio → gate, which the host adapts) is allowed.
[[nodiscard]] bool port_kinds_compatible(PortKind src, PortKind dst);

[[nodiscard]] const char* port_kind_name(PortKind kind);

struct Port {
    std::string id; // stable within the node ("ch01", "ch01.vol.cv", "main")
    std::string label;
    PortKind kind = PortKind::kAudio;
    // Engine channel index for tracker-bus ports (-1 elsewhere). Lets
    // the runner bind ports to engine buffers without string parsing.
    int channel_ref = -1;
    // Plugin-node CV param ports only: index into the bound instance's
    // parameter list (GraphPluginBinding::plugin_set_param_cv). -1
    // elsewhere. Never serialised — the port id (param key) is the
    // stable identity; the session regenerates indices with the ports.
    int param_ref = -1;
};

// Floating-window placement, persisted with the project (WPBR). Mirrors
// the web's WorkspaceWindowSnapshot (trackerEngine.ts:151-158).
struct WindowSnapshot {
    bool visible = true;
    float x = 80.0F;
    float y = 80.0F;
    float width = 0.0F; // 0 = let the window auto-size once
    float height = 0.0F;
    bool minimised = false;
    int z_index = 0;
};

struct Node {
    std::string workspace_id; // stable ("__tracker-bus", "sum-1", ...)
    std::string plugin_id;    // ("builtin:tracker-bus", ...)
    std::string display_name;
    NodeKind kind = NodeKind::kUtilitySum;
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    WindowSnapshot window;
    // Master-route strip (web: WorkspaceInstrumentSnapshot volume/pan/
    // bypass). Only meaningful for plugin nodes; carried for all nodes
    // so project state round-trips.
    float volume = 1.0F;
    float pan = 0.0F;
    bool bypass = false;
};

struct CableEnd {
    std::string node_id;
    std::string port_id;
};

struct Cable {
    std::string id;
    CableEnd source; // an output port
    CableEnd dest;   // an input port
    CableMode mode = CableMode::kTap;
    // Kinds cached at creation for round-trip validation on load (web
    // v4 srcKind/dstKind fields).
    PortKind src_kind = PortKind::kAudio;
    PortKind dst_kind = PortKind::kAudio;
};

// Typed connection outcome. Everything except kOk carries a
// user-facing message via connect_result_message().
enum class ConnectResult : std::uint8_t {
    kOk,
    kSourceNodeMissing,
    kDestNodeMissing,
    kSourcePortMissing,
    kDestPortMissing,
    kKindMismatch,
    kDuplicateCableId,
};

[[nodiscard]] const char* connect_result_message(ConnectResult result);

// The workspace graph model. Mutations validate; the audio thread never
// reads this — publish through graph_compile + GraphRunner.
class WorkspaceGraph {
public:
    // Adds a node; the caller guarantees a unique workspace_id (the
    // builders below use fixed ids for pseudo-nodes, next_node_id() for
    // user-created ones).
    Node& add_node(Node node);

    // Removes a node and every cable touching it (the web rips cables
    // before engine teardown; same order here). Returns false when the
    // id is unknown.
    bool remove_node(const std::string& workspace_id);

    [[nodiscard]] const Node* find_node(const std::string& workspace_id) const;
    [[nodiscard]] Node* find_node(const std::string& workspace_id);
    [[nodiscard]] int node_index(const std::string& workspace_id) const; // -1 = missing

    // Validated connect. On kOk the cable is appended; `cable_id` empty
    // generates a fresh unique id. Duplicate cables between the same
    // jacks are legal (physical patching semantics — double gain).
    ConnectResult connect(const CableEnd& source, const CableEnd& dest, CableMode mode,
                          const std::string& cable_id = std::string());

    bool disconnect(const std::string& cable_id);
    bool set_cable_mode(const std::string& cable_id, CableMode mode);
    [[nodiscard]] const Cable* find_cable(const std::string& cable_id) const;

    [[nodiscard]] const std::vector<Node>& nodes() const { return nodes_; }

    // Mutable node access for the workspace view's window write-back
    // (geometry only — structural changes go through the methods above).
    [[nodiscard]] std::vector<Node>& nodes_mut() { return nodes_; }

    [[nodiscard]] const std::vector<Cable>& cables() const { return cables_; }

    // Fresh unique ids ("sum-1", "cbl-1", ...). Counter-based — no
    // randomness, so tests and replays are deterministic.
    [[nodiscard]] std::string next_node_id(const std::string& prefix) const;
    [[nodiscard]] std::string next_cable_id() const;

private:
    std::vector<Node> nodes_;
    std::vector<Cable> cables_;
};

// Port lookup by stable id; nullptr when missing. `port_index` variants
// return -1 when missing.
[[nodiscard]] const Port* find_output(const Node& node, const std::string& port_id);
[[nodiscard]] const Port* find_input(const Node& node, const std::string& port_id);
[[nodiscard]] int output_index(const Node& node, const std::string& port_id);
[[nodiscard]] int input_index(const Node& node, const std::string& port_id);

// ── Built-in node builders ───────────────────────────────────────────

inline constexpr const char* kTrackerBusId = "__tracker-bus";
inline constexpr const char* kMasterInId = "__master-in";
inline constexpr const char* kModulePlayerId = "__module-player";

// Cap on generated CV param ports per plugin node (the parameter list
// can be huge on external plugins; the first N stay patchable).
inline constexpr int kMaxPluginCvParamPorts = 32;

// Tracker Bus: per tracker channel an audio out "chNN", a volume CV
// out "chNN.vol.cv" and a row-event MIDI out "chNN.midi", plus the
// merged "master.midi" out and the "master.midi.in" input (web v5
// layout and port ids kept verbatim — workspaceTrackerBus.ts — so
// portId- and jackIndex-based WPBR snapshots resolve 1:1).
[[nodiscard]] Node make_tracker_bus_node(int channel_count);

// Master In: single audio input "main" summing into the master bus.
[[nodiscard]] Node make_master_in_node();

// Module Player: stereo audio out "main" carrying the libopenmpt mix.
// Native-only node — the web app has no module player.
[[nodiscard]] Node make_module_player_node();

// Utility sum: audio "in" → audio "out" pass-through. Gives patches a
// mixing/branching point and makes feedback loops constructible.
[[nodiscard]] Node make_utility_sum_node(const std::string& workspace_id);

// Ext MIDI In / Out: user-creatable bridges between midi cables and
// the app's hardware MIDI devices (the MIDI window's open input /
// output — one device pair app-wide, every node mirrors it). In has a
// single midi out "midi"; Out a single midi in "midi".
[[nodiscard]] Node make_ext_midi_in_node(const std::string& workspace_id);
[[nodiscard]] Node make_ext_midi_out_node(const std::string& workspace_id);

} // namespace nt::graph
