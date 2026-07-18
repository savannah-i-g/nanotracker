// Workspace graph verification: the kind-compatibility matrix (exact
// web semantics), model validation, schedule compilation with cycle
// breaking, WPBR adoption/serialisation round-trips, and deterministic
// block evaluation through the runner (tap fan-out, reroute
// suppression, one-block feedback delay, CV fill, gate edge events,
// midi event transport with merge/fan-out and the Ext MIDI bridges).
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

namespace {

// Synthetic node with a single midi input — a cable sink the runner
// gathers into (kUtilitySum's evaluator ignores midi, which is exactly
// what a pure sink needs).
nt::graph::Node make_midi_sink(const char* workspace_id) {
    nt::graph::Node node;
    node.workspace_id = workspace_id;
    node.kind = nt::graph::NodeKind::kUtilitySum;
    nt::graph::Port in;
    in.id = "in";
    in.kind = PortKind::kMidi;
    node.inputs.push_back(in);
    return node;
}

nt::audio::MidiMessage note_on(std::uint32_t frame, std::uint8_t channel, std::uint8_t note,
                               std::uint8_t velocity) {
    return {.frame = frame,
            .type = nt::audio::MidiMessage::Type::kNoteOn,
            .channel = channel,
            .data1 = note,
            .data2 = velocity};
}

} // namespace

TEST_CASE("runner: midi cables fan out and merge sorted by frame", "[graph]") {
    WorkspaceGraph graph = make_default_graph(2);
    graph.add_node(make_midi_sink("merge"));
    graph.add_node(make_midi_sink("copy"));
    graph.add_node(make_midi_sink("master"));
    REQUIRE(graph.connect(bus_out("ch01.midi"), {.node_id = "merge", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect(bus_out("ch02.midi"), {.node_id = "merge", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect(bus_out("ch01.midi"), {.node_id = "copy", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);
    REQUIRE(graph.connect(bus_out("master.midi"), {.node_id = "master", .port_id = "in"},
                          CableMode::kTap) == ConnectResult::kOk);

    nt::audio::GraphRunner runner(graph, nt::graph::compile_graph(graph));

    nt::audio::MidiEventList ch0;
    ch0.push(note_on(10, 0, 60, 100));
    ch0.push(note_on(100, 0, 64, 90));
    nt::audio::MidiEventList ch1;
    ch1.push(note_on(40, 1, 67, 80));
    const std::array<const nt::audio::MidiEventList*, 2> channel_midi = {&ch0, &ch1};

    std::array<float, 256> silent{};
    const std::array<const float*, 2> scratch = {silent.data(), silent.data()};
    const std::array<float, 2> gains = {1.0F, 1.0F};
    std::array<float, 256> master{};
    const nt::audio::GraphBlockContext ctx{
        .channel_scratch = scratch.data(),
        .channel_count = 2,
        .channel_gains = gains.data(),
        .module_block = nullptr,
        .master_accum = master.data(),
        .channel_midi = channel_midi.data(),
    };
    runner.process(ctx, 128);

    // Fan-in: both channel lists merge, sorted by frame.
    const nt::audio::MidiEventList& merged = runner.debug_midi_input(graph.node_index("merge"), 0);
    REQUIRE(merged.count == 3);
    CHECK(merged.events[0].frame == 10);
    CHECK(merged.events[0].channel == 0);
    CHECK(merged.events[1].frame == 40);
    CHECK(merged.events[1].channel == 1);
    CHECK(merged.events[2].frame == 100);
    CHECK(merged.dropped == 0);

    // Fan-out: the second destination gets a full copy.
    const nt::audio::MidiEventList& copied = runner.debug_midi_input(graph.node_index("copy"), 0);
    REQUIRE(copied.count == 2);
    CHECK(copied.events[0].data1 == 60);
    CHECK(copied.events[1].data1 == 64);

    // master.midi carries every channel, merged and channel-stamped.
    const nt::audio::MidiEventList& all = runner.debug_midi_input(graph.node_index("master"), 0);
    REQUIRE(all.count == 3);
    CHECK(all.events[1].channel == 1);

    // The bus's chNN.midi output list is inspectable too.
    const int bus = graph.node_index(nt::graph::kTrackerBusId);
    CHECK(runner.debug_midi_output(bus, 2).count == 2); // ch01.midi
}

TEST_CASE("runner: ext midi nodes bridge rings and stamp stream time", "[graph]") {
    WorkspaceGraph graph = make_default_graph(1);
    graph.add_node(nt::graph::make_ext_midi_in_node("emi-1"));
    graph.add_node(nt::graph::make_ext_midi_out_node("emo-1"));
    REQUIRE(graph.connect({.node_id = "emi-1", .port_id = "midi"},
                          {.node_id = "emo-1", .port_id = "midi"},
                          CableMode::kTap) == ConnectResult::kOk);
    // Ext In also feeds the sequencer's record hook via master.midi.in.
    REQUIRE(graph.connect({.node_id = "emi-1", .port_id = "midi"},
                          {.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi.in"},
                          CableMode::kTap) == ConnectResult::kOk);

    nt::audio::GraphRunner runner(graph, nt::graph::compile_graph(graph));

    nt::audio::MidiEventList arrivals;
    arrivals.push(note_on(0, 2, 69, 101));
    arrivals.push({.frame = 0,
                   .type = nt::audio::MidiMessage::Type::kControlChange,
                   .channel = 2,
                   .data1 = 74,
                   .data2 = 42});
    nt::rt::SpscQueue<nt::audio::ExtMidiOutMessage> wire{16};
    nt::rt::SpscQueue<nt::audio::TimedMidiMessage> record{16};

    std::array<float, 256> silent{};
    const std::array<const float*, 1> scratch = {silent.data()};
    const std::array<float, 1> gains = {1.0F};
    std::array<float, 256> master{};
    const nt::audio::GraphBlockContext ctx{
        .channel_scratch = scratch.data(),
        .channel_count = 1,
        .channel_gains = gains.data(),
        .module_block = nullptr,
        .master_accum = master.data(),
        .ext_midi_in = &arrivals,
        .ext_midi_out = &wire,
        .bus_midi_in = &record,
        .block_start_frame = 4096,
    };
    runner.process(ctx, 128);

    // Ext Out: encoded wire bytes, timestamped block start + frame.
    nt::audio::ExtMidiOutMessage out{};
    REQUIRE(wire.pop(out));
    CHECK(out.size == 3);
    CHECK(out.bytes[0] == 0x92); // note on, channel 2
    CHECK(out.bytes[1] == 69);
    CHECK(out.bytes[2] == 101);
    CHECK(out.at_sample == 4096);
    REQUIRE(wire.pop(out));
    CHECK(out.bytes[0] == 0xB2);
    CHECK(out.bytes[1] == 74);
    CHECK(out.bytes[2] == 42);
    CHECK_FALSE(wire.pop(out));

    // master.midi.in: same events into the record hook with absolute
    // stream frames.
    nt::audio::TimedMidiMessage timed{};
    REQUIRE(record.pop(timed));
    CHECK(timed.message.type == nt::audio::MidiMessage::Type::kNoteOn);
    CHECK(timed.at_frame == 4096);
    REQUIRE(record.pop(timed));
    CHECK(timed.message.type == nt::audio::MidiMessage::Type::kControlChange);
    CHECK_FALSE(record.pop(timed));
}

TEST_CASE("compile schedules midi edges; midi cycles break with a delay", "[graph]") {
    WorkspaceGraph graph = make_default_graph(1);
    graph.add_node(nt::graph::make_ext_midi_in_node("emi-1"));
    graph.add_node(nt::graph::make_ext_midi_out_node("emo-1"));
    REQUIRE(graph.connect({.node_id = "emi-1", .port_id = "midi"},
                          {.node_id = "emo-1", .port_id = "midi"},
                          CableMode::kTap) == ConnectResult::kOk);
    // Self-loop through the bus: master.midi → master.midi.in.
    REQUIRE(graph.connect(bus_out("master.midi"),
                          {.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi.in"},
                          CableMode::kTap) == ConnectResult::kOk);

    const nt::graph::GraphSchedule schedule = nt::graph::compile_graph(graph);
    REQUIRE(schedule.edges.size() == 2); // midi edges are scheduled now
    int delayed = 0;
    for (const nt::graph::ScheduleEdge& edge : schedule.edges) {
        CHECK(edge.src_kind == PortKind::kMidi);
        delayed += edge.delayed ? 1 : 0;
    }
    CHECK(delayed == 1); // the bus self-loop
}

TEST_CASE("WPBR: ext midi nodes and midi cables round-trip", "[graph]") {
    WorkspaceGraph graph = make_default_graph(2);
    graph.add_node(nt::graph::make_ext_midi_in_node("emi-1"));
    graph.add_node(nt::graph::make_ext_midi_out_node("emo-1"));
    REQUIRE(graph.connect(bus_out("ch01.midi"), {.node_id = "emo-1", .port_id = "midi"},
                          CableMode::kTap, "c-bus") == ConnectResult::kOk);
    REQUIRE(graph.connect({.node_id = "emi-1", .port_id = "midi"},
                          {.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi.in"},
                          CableMode::kTap, "c-in") == ConnectResult::kOk);
    graph.find_node("emi-1")->window.x = 321.0F;

    const std::string serialised =
        nt::graph::workspace_to_wpbr_json(graph, nt::graph::DormantEntries{});

    WorkspaceGraph second = make_default_graph(2);
    const nt::graph::WpbrAdoptResult adopted = nt::graph::adopt_wpbr_json(second, serialised);
    CHECK(adopted.warnings.empty());
    CHECK(adopted.dormant.cables.empty());
    const nt::graph::Node* emi = second.find_node("emi-1");
    REQUIRE(emi != nullptr);
    CHECK(emi->kind == nt::graph::NodeKind::kExtMidiIn);
    CHECK(emi->window.x == 321.0F);
    REQUIRE(second.find_node("emo-1") != nullptr);
    CHECK(second.find_node("emo-1")->kind == nt::graph::NodeKind::kExtMidiOut);
    const nt::graph::Cable* c_bus = second.find_cable("c-bus");
    REQUIRE(c_bus != nullptr);
    CHECK(c_bus->src_kind == PortKind::kMidi);
    CHECK(c_bus->source.port_id == "ch01.midi");
    REQUIRE(second.find_cable("c-in") != nullptr);
    CHECK(second.find_cable("c-in")->dest.port_id == "master.midi.in");
}

TEST_CASE("WPBR: v5 jackIndex midi endpoints resolve on the live bus", "[graph]") {
    WorkspaceGraph graph = make_default_graph(4);
    // v5 typed era: jackIndex 2 = ch01.midi; channels*3 = master.midi;
    // dest jackIndex on the bus = master.midi.in.
    const std::string payload = R"({
      "instruments": [],
      "cables": [
        {"id": "c-ch-midi", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 2},
         "dest": {"workspaceId": "__tracker-bus", "jackIndex": 0},
         "srcKind": "midi", "dstKind": "midi"},
        {"id": "c-master-midi", "mode": "tap",
         "source": {"workspaceId": "__tracker-bus", "jackIndex": 12},
         "dest": {"workspaceId": "__tracker-bus", "jackIndex": 0},
         "srcKind": "midi", "dstKind": "midi"}
      ]
    })";
    const nt::graph::WpbrAdoptResult result = nt::graph::adopt_wpbr_json(graph, payload);
    CHECK(result.warnings.empty());
    CHECK(result.dormant.cables.empty());
    REQUIRE(graph.find_cable("c-ch-midi") != nullptr);
    CHECK(graph.find_cable("c-ch-midi")->source.port_id == "ch01.midi");
    CHECK(graph.find_cable("c-ch-midi")->dest.port_id == "master.midi.in");
    REQUIRE(graph.find_cable("c-master-midi") != nullptr);
    CHECK(graph.find_cable("c-master-midi")->source.port_id == "master.midi");
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
