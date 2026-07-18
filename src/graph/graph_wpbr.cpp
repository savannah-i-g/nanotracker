#include "graph/graph_wpbr.h"

#include <nlohmann/json.hpp>

#include <optional>

namespace nt::graph {

namespace {

using nlohmann::json;

std::optional<PortKind> kind_from_name(const std::string& name) {
    if (name == "audio") {
        return PortKind::kAudio;
    }
    if (name == "sidechain") {
        return PortKind::kSidechain;
    }
    if (name == "cv") {
        return PortKind::kCv;
    }
    if (name == "gate") {
        return PortKind::kGate;
    }
    if (name == "midi") {
        return PortKind::kMidi;
    }
    return std::nullopt;
}

WindowSnapshot window_from_json(const json& j) {
    WindowSnapshot w;
    w.visible = j.value("visible", true);
    w.x = j.value("x", 80.0F);
    w.y = j.value("y", 80.0F);
    w.width = j.value("width", 0.0F);
    w.height = j.value("height", 0.0F);
    w.minimised = j.value("minimised", false);
    w.z_index = j.value("zIndex", 0);
    return w;
}

json window_to_json(const WindowSnapshot& w) {
    return json{{"visible", w.visible}, {"x", w.x},           {"y", w.y},
                {"width", w.width},     {"height", w.height}, {"minimised", w.minimised},
                {"zIndex", w.z_index}};
}

// Result of mapping one web endpoint onto the native model.
struct ResolvedEnd {
    enum class State : std::uint8_t { kOk, kDormant, kDropped };
    State state = State::kDropped;
    CableEnd end;
    std::string why; // set for kDropped
};

// Maps a web jackIndex onto a stable port id using the web's
// historical tracker-bus layouts (see header). `want` is the stored
// kind when the snapshot has one — it arbitrates between layouts.
// The native bus now carries the full v5 layout ([audio, vol cv,
// midi] per channel + trailing master.midi), so every era resolves to
// a live port.
ResolvedEnd resolve_bus_output_by_index(const Node& bus, int jack_index,
                                        std::optional<PortKind> want) {
    const int channels = static_cast<int>(bus.outputs.size()) / 3;

    struct Candidate {
        int channel;
        int role; // 0 = audio, 1 = vol cv, 2 = midi, 3 = master.midi
    };

    std::vector<Candidate> candidates;
    if (want.has_value()) {
        // v5 layout: 3 ports per channel + trailing master.midi.
        if (jack_index == channels * 3) {
            candidates.push_back({.channel = -1, .role = 3});
        } else {
            candidates.push_back({.channel = jack_index / 3, .role = jack_index % 3});
        }
        // v4 layout: 2 ports per channel.
        candidates.push_back({.channel = jack_index / 2, .role = jack_index % 2});
    } else {
        // Legacy outputs[] was one gain per channel.
        candidates.push_back({.channel = jack_index, .role = 0});
    }
    for (const Candidate& c : candidates) {
        PortKind kind = PortKind::kMidi;
        if (c.role == 0) {
            kind = PortKind::kAudio;
        } else if (c.role == 1) {
            kind = PortKind::kCv;
        }
        if (want.has_value() && *want != kind) {
            continue;
        }
        if (c.role == 3) {
            return {.state = ResolvedEnd::State::kOk,
                    .end = {.node_id = bus.workspace_id,
                            .port_id = bus.outputs[static_cast<std::size_t>(channels) * 3].id},
                    .why = {}};
        }
        if (c.channel < 0 || c.channel >= channels) {
            continue;
        }
        const std::size_t index =
            (static_cast<std::size_t>(c.channel) * 3) + static_cast<std::size_t>(c.role);
        return {.state = ResolvedEnd::State::kOk,
                .end = {.node_id = bus.workspace_id, .port_id = bus.outputs[index].id},
                .why = {}};
    }
    return {.state = ResolvedEnd::State::kDropped,
            .end = {},
            .why = "jack index " + std::to_string(jack_index) + " unmappable on tracker bus"};
}

ResolvedEnd resolve_endpoint(const WorkspaceGraph& graph, const json& endpoint, bool is_source,
                             std::optional<PortKind> want) {
    const std::string workspace_id = endpoint.value("workspaceId", "");
    const Node* node = graph.find_node(workspace_id);
    if (node == nullptr) {
        // Unknown instrument — typically an NTP plugin awaiting its
        // stage. Keep the cable dormant rather than dropping it.
        return {.state = ResolvedEnd::State::kDormant, .end = {}, .why = {}};
    }

    if (endpoint.contains("portId") && endpoint["portId"].is_string()) {
        const std::string port_id = endpoint["portId"].get<std::string>();
        const Port* port = is_source ? find_output(*node, port_id) : find_input(*node, port_id);
        if (port != nullptr) {
            return {.state = ResolvedEnd::State::kOk,
                    .end = {.node_id = workspace_id, .port_id = port_id},
                    .why = {}};
        }
        if (want == PortKind::kMidi) {
            // Midi ports the native node set doesn't expose (the web's
            // implicit "midi-thru"/"midi-out" adapter ports) — keep
            // the cable dormant so projects round-trip losslessly.
            return {.state = ResolvedEnd::State::kDormant, .end = {}, .why = {}};
        }
        return {.state = ResolvedEnd::State::kDropped,
                .end = {},
                .why = "port \"" + port_id + "\" not found on \"" + workspace_id + "\""};
    }

    const int jack_index = endpoint.value("jackIndex", -1);
    if (jack_index < 0) {
        return {.state = ResolvedEnd::State::kDropped,
                .end = {},
                .why = "endpoint on \"" + workspace_id + "\" has no portId or jackIndex"};
    }
    if (node->kind == NodeKind::kTrackerBus) {
        if (!is_source) {
            // The bus's only input, in every era: master.midi.in.
            if (!node->inputs.empty()) {
                return {.state = ResolvedEnd::State::kOk,
                        .end = {.node_id = workspace_id, .port_id = node->inputs.front().id},
                        .why = {}};
            }
            return {.state = ResolvedEnd::State::kDropped,
                    .end = {},
                    .why = "tracker bus has no inputs"};
        }
        return resolve_bus_output_by_index(*node, jack_index, want);
    }
    // Every other node type has always had a flat jack list; index maps
    // positionally.
    const std::vector<Port>& ports = is_source ? node->outputs : node->inputs;
    if (jack_index < static_cast<int>(ports.size())) {
        return {.state = ResolvedEnd::State::kOk,
                .end = {.node_id = workspace_id,
                        .port_id = ports[static_cast<std::size_t>(jack_index)].id},
                .why = {}};
    }
    return {.state = ResolvedEnd::State::kDropped,
            .end = {},
            .why = "jack index " + std::to_string(jack_index) + " out of range on \"" +
                   workspace_id + "\""};
}

} // namespace

WpbrAdoptResult adopt_wpbr_json(WorkspaceGraph& graph, const std::string& payload) {
    WpbrAdoptResult result;
    if (payload.empty()) {
        return result;
    }
    const json root = json::parse(payload, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.warnings.emplace_back("workspace: WPBR payload is not valid JSON — ignored");
        return result;
    }

    for (const json& inst : root.value("instruments", json::array())) {
        if (!inst.is_object()) {
            continue;
        }
        const std::string workspace_id = inst.value("workspaceId", "");
        const std::string plugin_id = inst.value("pluginId", "");
        Node* node = graph.find_node(workspace_id);
        if (node == nullptr && plugin_id == "builtin:sum") {
            // Native-authored utility node — recreate it.
            node = &graph.add_node(make_utility_sum_node(workspace_id));
        }
        if (node == nullptr && plugin_id == "builtin:ext-midi-in") {
            node = &graph.add_node(make_ext_midi_in_node(workspace_id));
        }
        if (node == nullptr && plugin_id == "builtin:ext-midi-out") {
            node = &graph.add_node(make_ext_midi_out_node(workspace_id));
        }
        if (node == nullptr) {
            // Plugin instrument awaiting its stage: carry verbatim.
            result.dormant.instruments.push_back(inst.dump());
            continue;
        }
        if (inst.contains("windowState") && inst["windowState"].is_object()) {
            node->window = window_from_json(inst["windowState"]);
        }
        node->volume = inst.value("volume", 1.0F);
        node->pan = inst.value("pan", 0.0F);
        node->bypass = inst.value("bypass", false);
    }

    for (const json& cable : root.value("cables", json::array())) {
        if (!cable.is_object() || !cable.contains("source") || !cable.contains("dest")) {
            continue;
        }
        std::optional<PortKind> src_kind;
        std::optional<PortKind> dst_kind;
        if (cable.contains("srcKind") && cable["srcKind"].is_string()) {
            src_kind = kind_from_name(cable["srcKind"].get<std::string>());
        }
        if (cable.contains("dstKind") && cable["dstKind"].is_string()) {
            dst_kind = kind_from_name(cable["dstKind"].get<std::string>());
        }
        const ResolvedEnd src = resolve_endpoint(graph, cable["source"], true, src_kind);
        const ResolvedEnd dst = resolve_endpoint(graph, cable["dest"], false, dst_kind);
        if (src.state == ResolvedEnd::State::kDormant ||
            dst.state == ResolvedEnd::State::kDormant) {
            result.dormant.cables.push_back(cable.dump());
            continue;
        }
        const std::string cable_id = cable.value("id", "");
        if (src.state == ResolvedEnd::State::kDropped ||
            dst.state == ResolvedEnd::State::kDropped) {
            result.warnings.push_back(
                "workspace: cable \"" + cable_id + "\" dropped — " +
                (src.state == ResolvedEnd::State::kDropped ? src.why : dst.why));
            continue;
        }
        const CableMode mode = cable.value("mode", "tap") == std::string("reroute")
                                   ? CableMode::kReroute
                                   : CableMode::kTap;
        const ConnectResult connected = graph.connect(src.end, dst.end, mode, cable_id);
        if (connected != ConnectResult::kOk) {
            result.warnings.push_back("workspace: cable \"" + cable_id + "\" dropped — " +
                                      connect_result_message(connected));
        }
    }
    return result;
}

std::string workspace_to_wpbr_json(const WorkspaceGraph& graph, const DormantEntries& dormant) {
    json instruments = json::array();
    for (const Node& node : graph.nodes()) {
        instruments.push_back(json{{"workspaceId", node.workspace_id},
                                   {"pluginId", node.plugin_id},
                                   {"displayName", node.display_name},
                                   {"paramSnapshot", json::object()},
                                   {"windowState", window_to_json(node.window)},
                                   {"volume", node.volume},
                                   {"pan", node.pan},
                                   {"bypass", node.bypass}});
    }
    for (const std::string& raw : dormant.instruments) {
        json parsed = json::parse(raw, nullptr, false);
        if (!parsed.is_discarded()) {
            instruments.push_back(std::move(parsed));
        }
    }

    json cables = json::array();
    for (const Cable& cable : graph.cables()) {
        const Node* src_node = graph.find_node(cable.source.node_id);
        const Node* dst_node = graph.find_node(cable.dest.node_id);
        if (src_node == nullptr || dst_node == nullptr) {
            continue;
        }
        // portId is authoritative; jackIndex is the native list index,
        // emitted alongside per the web v4.1 writer contract.
        cables.push_back(json{
            {"id", cable.id},
            {"source",
             {{"workspaceId", cable.source.node_id},
              {"jackIndex", output_index(*src_node, cable.source.port_id)},
              {"portId", cable.source.port_id}}},
            {"dest",
             {{"workspaceId", cable.dest.node_id},
              {"jackIndex", input_index(*dst_node, cable.dest.port_id)},
              {"portId", cable.dest.port_id}}},
            {"mode", cable.mode == CableMode::kReroute ? "reroute" : "tap"},
            {"srcKind", port_kind_name(cable.src_kind)},
            {"dstKind", port_kind_name(cable.dst_kind)},
        });
    }
    for (const std::string& raw : dormant.cables) {
        json parsed = json::parse(raw, nullptr, false);
        if (!parsed.is_discarded()) {
            cables.push_back(std::move(parsed));
        }
    }

    return json{{"instruments", std::move(instruments)}, {"cables", std::move(cables)}}.dump();
}

} // namespace nt::graph
