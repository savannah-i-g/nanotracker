// Workspace graph verification: the kind-compatibility matrix (exact
// web semantics), model validation, schedule compilation with cycle
// breaking, WPBR adoption/serialisation round-trips, and deterministic
// block evaluation through the runner (tap fan-out, reroute
// suppression, one-block feedback delay, CV fill, gate edge events).
#include "audio/graph_runner.h"
#include "graph/graph_compile.h"
#include "graph/graph_model.h"
#include "graph/graph_wpbr.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace {

using nt::graph::CableEnd;
using nt::graph::CableMode;
using nt::graph::ConnectResult;
using nt::graph::PortKind;
using nt::graph::WorkspaceGraph;

WorkspaceGraph make_default_graph(int channels = 4) {
    WorkspaceGraph graph;
    graph.add_node(nt::graph::make_tracker_bus_node(channels));
    graph.add_node(nt::graph::make_master_in_node());
    graph.add_node(nt::graph::make_module_player_node());
    return graph;
}

CableEnd bus_out(const char* port) {
    return {.node_id = nt::graph::kTrackerBusId, .port_id = port};
}

CableEnd master_in() {
    return {.node_id = nt::graph::kMasterInId, .port_id = "main"};
}

} // namespace

TEST_CASE("port kind compatibility matches the web matrix", "[graph]") {
    using enum PortKind;
    const std::array<PortKind, 5> kinds = {kAudio, kSidechain, kCv, kGate, kMidi};
    // Truth table transcribed from workspaceCableGraph.ts:94-104,
    // rows = src, cols = dst in the order above.
    constexpr std::array<std::array<bool, 5>, 5> expected = {{
        {true, true, true, true, false},    // audio →
        {true, true, true, true, false},    // sidechain →
        {false, false, true, false, false}, // cv →
        {false, false, false, true, false}, // gate →
        {false, false, false, false, true}, // midi →
    }};
    for (std::size_t s = 0; s < kinds.size(); ++s) {
        for (std::size_t d = 0; d < kinds.size(); ++d) {
            INFO("src=" << nt::graph::port_kind_name(kinds[s])
                        << " dst=" << nt::graph::port_kind_name(kinds[d]));
            CHECK(nt::graph::port_kinds_compatible(kinds[s], kinds[d]) == expected[s][d]);
        }
    }
}

TEST_CASE("connect validates endpoints and kinds", "[graph]") {
    WorkspaceGraph graph = make_default_graph();

    CHECK(graph.connect(bus_out("ch01"), master_in(), CableMode::kTap) == ConnectResult::kOk);
    CHECK(graph.connect({.node_id = "ghost", .port_id = "x"}, master_in(), CableMode::kTap) ==
          ConnectResult::kSourceNodeMissing);
    CHECK(graph.connect(bus_out("ch01"), {.node_id = "ghost", .port_id = "x"}, CableMode::kTap) ==
          ConnectResult::kDestNodeMissing);
    CHECK(graph.connect(bus_out("ch99"), master_in(), CableMode::kTap) ==
          ConnectResult::kSourcePortMissing);
    CHECK(graph.connect(bus_out("ch01"), {.node_id = nt::graph::kMasterInId, .port_id = "side"},
                        CableMode::kTap) == ConnectResult::kDestPortMissing);
    // cv → audio is rejected by the matrix.
    CHECK(graph.connect(bus_out("ch01.vol.cv"), master_in(), CableMode::kTap) ==
          ConnectResult::kKindMismatch);
    // Duplicate cable ids refuse; duplicate edges are legal.
    CHECK(graph.connect(bus_out("ch02"), master_in(), CableMode::kTap, "cbl-1") ==
          ConnectResult::kDuplicateCableId);
    CHECK(graph.connect(bus_out("ch01"), master_in(), CableMode::kTap) == ConnectResult::kOk);
    CHECK(graph.cables().size() == 2);
}

TEST_CASE("node removal rips touching cables", "[graph]") {
    WorkspaceGraph graph = make_default_graph();
    graph.add_node(nt::graph::make_utility_sum_node("sum-1"));
    REQUIRE(graph.connect(bus_out("ch01"), {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"}, master_in(), CableMode::kTap) ==
            ConnectResult::kOk);
    REQUIRE(graph.connect(bus_out("ch02"), master_in(), CableMode::kTap) == ConnectResult::kOk);

    CHECK(graph.remove_node("sum-1"));
    CHECK(graph.cables().size() == 1); // only the direct ch02 cable survives
    CHECK(graph.find_node("sum-1") == nullptr);
}

TEST_CASE("compile resolves order, suppression and feedback delay", "[graph]") {
    WorkspaceGraph graph = make_default_graph();
    graph.add_node(nt::graph::make_utility_sum_node("sum-1"));
    graph.add_node(nt::graph::make_utility_sum_node("sum-2"));

    REQUIRE(graph.connect(bus_out("ch01"), {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"},
                          {.node_id = "sum-2", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-2", .port_id = "out"}, master_in(), CableMode::kTap) ==
            ConnectResult::kOk);
    // Feedback: sum-2 → sum-1 closes a cycle.
    REQUIRE(graph.connect({.node_id = "sum-2", .port_id = "out"},
                          {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    // Reroute cables drive the suppression outputs.
    REQUIRE(graph.connect(bus_out("ch03"), master_in(), CableMode::kReroute) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = nt::graph::kModulePlayerId, .port_id = "main"}, master_in(),
                          CableMode::kReroute) == ConnectResult::kOk);

    const nt::graph::GraphSchedule schedule = nt::graph::compile_graph(graph);

    CHECK(schedule.suppressed_channel_mask == 0b100U); // channel 3 (index 2)
    CHECK(schedule.module_suppressed);

    int delayed = 0;
    for (const nt::graph::ScheduleEdge& edge : schedule.edges) {
        delayed += edge.delayed ? 1 : 0;
    }
    CHECK(delayed == 1); // exactly one edge breaks the cycle

    // Topological check: every non-delayed edge's source evaluates
    // before its destination.
    std::vector<int> position(graph.nodes().size(), -1);
    for (std::size_t i = 0; i < schedule.order.size(); ++i) {
        position[static_cast<std::size_t>(schedule.order[i])] = static_cast<int>(i);
    }
    for (const nt::graph::ScheduleEdge& edge : schedule.edges) {
        if (!edge.delayed) {
            INFO("edge " << edge.src_node << " -> " << edge.dst_node);
            CHECK(position[static_cast<std::size_t>(edge.src_node)] <
                  position[static_cast<std::size_t>(edge.dst_node)]);
        }
    }
}

TEST_CASE("self-loop compiles to a delayed edge", "[graph]") {
    WorkspaceGraph graph;
    graph.add_node(nt::graph::make_utility_sum_node("sum-1"));
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"},
                          {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    const nt::graph::GraphSchedule schedule = nt::graph::compile_graph(graph);
    REQUIRE(schedule.edges.size() == 1);
    CHECK(schedule.edges[0].delayed);
}

TEST_CASE("runner: tap routes channel audio to master, reroute mask reported", "[graph]") {
    WorkspaceGraph graph = make_default_graph(2);
    REQUIRE(graph.connect(bus_out("ch01"), master_in(), CableMode::kTap) == ConnectResult::kOk);
    nt::audio::GraphRunner runner(graph, nt::graph::compile_graph(graph));

    std::array<float, 256> ch0{};
    std::array<float, 256> ch1{};
    ch0[0] = 1.0F;
    ch0[1] = 0.5F;
    ch1[0] = 0.25F; // not cabled — must not reach master via the graph
    const std::array<const float*, 2> scratch = {ch0.data(), ch1.data()};
    const std::array<float, 2> gains = {0.8F, 0.0F};
    std::array<float, 256> master{};

    const nt::audio::GraphBlockContext ctx{
        .channel_scratch = scratch.data(),
        .channel_count = 2,
        .channel_gains = gains.data(),
        .module_block = nullptr,
        .master_accum = master.data(),
    };
    runner.process(ctx, 128);

    CHECK(master[0] == 1.0F);
    CHECK(master[1] == 0.5F);
    for (std::size_t i = 2; i < master.size(); ++i) {
        CHECK(master[i] == 0.0F);
    }
    CHECK(runner.suppressed_channel_mask() == 0U);

    // CV out carries the channel gain for the whole block.
    const int bus = graph.node_index(nt::graph::kTrackerBusId);
    const float* cv = runner.debug_output(bus, 1); // ch01.vol.cv
    CHECK(cv[0] == 0.8F);
    CHECK(cv[127] == 0.8F);
}

TEST_CASE("runner: feedback loop rings with exactly one block of delay", "[graph]") {
    WorkspaceGraph graph = make_default_graph(1);
    graph.add_node(nt::graph::make_utility_sum_node("sum-1"));
    graph.add_node(nt::graph::make_utility_sum_node("sum-2"));
    REQUIRE(graph.connect(bus_out("ch01"), {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"},
                          {.node_id = "sum-2", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-2", .port_id = "out"},
                          {.node_id = "sum-1", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"}, master_in(), CableMode::kTap) ==
            ConnectResult::kOk);

    nt::audio::GraphRunner runner(graph, nt::graph::compile_graph(graph));

    std::array<float, 256> ch0{};
    const std::array<const float*, 1> scratch = {ch0.data()};
    const std::array<float, 1> gains = {1.0F};
    std::array<float, 256> master{};
    nt::audio::GraphBlockContext ctx{
        .channel_scratch = scratch.data(),
        .channel_count = 1,
        .channel_gains = gains.data(),
        .module_block = nullptr,
        .master_accum = master.data(),
    };

    // Block 1: impulse enters; the loop contribution is still silent.
    ch0[0] = 1.0F;
    runner.process(ctx, 128);
    CHECK(master[0] == 1.0F);

    // Block 2: input silent; the impulse comes back around the loop
    // (sum-2's previous block) at unity gain, same frame offset.
    ch0[0] = 0.0F;
    master.fill(0.0F);
    runner.process(ctx, 128);
    CHECK(master[0] == 1.0F);

    // Block 3: still ringing — the loop sustains it.
    master.fill(0.0F);
    runner.process(ctx, 128);
    CHECK(master[0] == 1.0F);
}

TEST_CASE("runner: audio drives gate inputs with frame-accurate edges", "[graph]") {
    // Synthetic node with a gate input, fed by a bus audio out.
    WorkspaceGraph graph = make_default_graph(1);
    nt::graph::Node gated;
    gated.workspace_id = "gated";
    gated.kind = nt::graph::NodeKind::kUtilitySum;
    nt::graph::Port gate_in;
    gate_in.id = "trig";
    gate_in.kind = PortKind::kGate;
    gated.inputs.push_back(gate_in);
    graph.add_node(gated);
    REQUIRE(graph.connect(bus_out("ch01"), {.node_id = "gated", .port_id = "trig"},
                          CableMode::kTap) == ConnectResult::kOk);

    nt::audio::GraphRunner runner(graph, nt::graph::compile_graph(graph));

    std::array<float, 256> ch0{};
    for (std::size_t i = 40; i < 90; ++i) {
        ch0[i * 2] = 0.9F; // high from frame 40 to 89
        ch0[(i * 2) + 1] = 0.9F;
    }
    const std::array<const float*, 1> scratch = {ch0.data()};
    const std::array<float, 1> gains = {1.0F};
    std::array<float, 256> master{};
    const nt::audio::GraphBlockContext ctx{
        .channel_scratch = scratch.data(),
        .channel_count = 1,
        .channel_gains = gains.data(),
        .module_block = nullptr,
        .master_accum = master.data(),
    };
    runner.process(ctx, 128);

    const int node = graph.node_index("gated");
    const nt::audio::GateEventList& events = runner.debug_gate_input(node, 0);
    REQUIRE(events.count == 2);
    CHECK(events.events[0].frame == 40);
    CHECK(events.events[0].on);
    CHECK(events.events[1].frame == 90);
    CHECK_FALSE(events.events[1].on);
}

TEST_CASE("WPBR adoption: portId, jackIndex eras, dormant carry", "[graph]") {
    WorkspaceGraph graph = make_default_graph(4);

    const std::string payload = R"({
      "instruments": [
        {"workspaceId": "__tracker-bus", "pluginId": "builtin:tracker-bus",
         "windowState": {"visible": true, "x": 11, "y": 22, "width": 200,
                          "height": 120, "minimised": true, "zIndex": 7}},
        {"workspaceId": "plg-123", "pluginId": "user:mysynth",
         "paramSnapshot": {"cutoff": 0.5}}
      ],
      "cables": [
        {"id": "c-portid", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 99, "portId": "ch02"},
         "dest": {"workspaceId": "__master-in", "jackIndex": 99, "portId": "main"},
         "srcKind": "audio", "dstKind": "audio"},
        {"id": "c-v5index", "mode": "reroute",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 3},
         "dest": {"workspaceId": "__master-in", "jackIndex": 0},
         "srcKind": "audio", "dstKind": "audio"},
        {"id": "c-legacy", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 2},
         "dest": {"workspaceId": "__master-in", "jackIndex": 0}},
        {"id": "c-midi", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 2},
         "dest": {"workspaceId": "plg-123", "jackIndex": 0},
         "srcKind": "midi", "dstKind": "midi"},
        {"id": "c-plugin", "mode": "tap",
         "source": {"workspaceId": "plg-123", "jackIndex": 0},
         "dest": {"workspaceId": "__master-in", "jackIndex": 0},
         "srcKind": "audio", "dstKind": "audio"},
        {"id": "c-bad", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "portId": "chXX"},
         "dest": {"workspaceId": "__master-in", "portId": "main"},
         "srcKind": "audio", "dstKind": "audio"}
      ]
    })";

    const nt::graph::WpbrAdoptResult result = nt::graph::adopt_wpbr_json(graph, payload);

    // Window placement adopted onto the bus.
    const nt::graph::Node* bus = graph.find_node(nt::graph::kTrackerBusId);
    REQUIRE(bus != nullptr);
    CHECK(bus->window.x == 11.0F);
    CHECK(bus->window.minimised);
    CHECK(bus->window.z_index == 7);

    // c-portid resolves by stable id despite the bogus jackIndex.
    const nt::graph::Cable* by_port = graph.find_cable("c-portid");
    REQUIRE(by_port != nullptr);
    CHECK(by_port->source.port_id == "ch02");

    // c-v5index: typed-era jackIndex 3 = ch02 audio in the v5 layout
    // (3 ports per channel: index 3 → channel 2, role audio).
    const nt::graph::Cable* by_index = graph.find_cable("c-v5index");
    REQUIRE(by_index != nullptr);
    CHECK(by_index->source.port_id == "ch02");
    CHECK(by_index->mode == CableMode::kReroute);

    // c-legacy: no kinds → legacy outputs[] was one jack per channel,
    // so index 2 = ch03.
    const nt::graph::Cable* legacy = graph.find_cable("c-legacy");
    REQUIRE(legacy != nullptr);
    CHECK(legacy->source.port_id == "ch03");

    // MIDI cable and plugin instrument/cable stay dormant; the bad
    // portId is dropped with a warning.
    CHECK(result.dormant.instruments.size() == 1);
    CHECK(result.dormant.cables.size() == 2); // c-midi + c-plugin
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("c-bad") != std::string::npos);
}

TEST_CASE("WPBR serialisation round-trips including dormant entries", "[graph]") {
    WorkspaceGraph graph = make_default_graph(2);
    graph.add_node(nt::graph::make_utility_sum_node("sum-1"));
    REQUIRE(graph.connect(bus_out("ch01"), {.node_id = "sum-1", .port_id = "in"}, CableMode::kTap,
                          "c-1") == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "sum-1", .port_id = "out"}, master_in(), CableMode::kReroute,
                          "c-2") == ConnectResult::kOk);
    graph.find_node("sum-1")->window.x = 123.0F;

    nt::graph::DormantEntries dormant;
    dormant.instruments.push_back(R"({"workspaceId":"plg-1","pluginId":"user:x"})");
    dormant.cables.push_back(R"({"id":"c-d","source":{"workspaceId":"plg-1","jackIndex":0},)"
                             R"("dest":{"workspaceId":"__master-in","jackIndex":0},"mode":"tap"})");

    const std::string serialised = nt::graph::workspace_to_wpbr_json(graph, dormant);

    // Re-adopt into a fresh default graph: the sum node, both live
    // cables and the dormant entries must all survive.
    WorkspaceGraph second = make_default_graph(2);
    const nt::graph::WpbrAdoptResult adopted = nt::graph::adopt_wpbr_json(second, serialised);
    CHECK(adopted.warnings.empty());
    REQUIRE(second.find_node("sum-1") != nullptr);
    CHECK(second.find_node("sum-1")->window.x == 123.0F);
    REQUIRE(second.find_cable("c-1") != nullptr);
    CHECK(second.find_cable("c-1")->dest.port_id == "in");
    REQUIRE(second.find_cable("c-2") != nullptr);
    CHECK(second.find_cable("c-2")->mode == CableMode::kReroute);
    CHECK(adopted.dormant.instruments.size() == 1);
    CHECK(adopted.dormant.cables.size() == 1);
}
