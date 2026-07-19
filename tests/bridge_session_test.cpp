// Stage 29e — the out-of-process bridge made session-facing. Where
// bridge_test.cpp drives the BridgedPlugin binding in isolation, this
// exercises it through ProjectSession: the per-node opt-in, the graph
// binding, the crash badge/restart entry points, and XPLG persistence.
//
// Coverage:
//   - the XPLG `bridged` flag round-trips through the io layer, and a file
//     written without it (the pre-S29e layout) reads back false — the
//     additive-field back-compat contract, testable with no bridge host;
//   - an old-style project (external record, bridged=false) loads into a
//     session as an in-process CLAP node (never bridged);
//   - a bridged CLAP node binds into the graph and produces audio through
//     the runner (device-gated: WARN-skips where the child can't spawn);
//   - toggling bridged<->in-process re-creates the instance cleanly and
//     carries its state across;
//   - the session-level crash -> badge-state -> restart path: the reaper
//     runs through update_bridged(), live_state() latches crashed, and
//     restart_bridged_plugin() recovers to live;
//   - a full session save with a plugin bridged reopens it bridged.
#include "app/project_session.h"
#include "engine/tracker_engine.h"
#include "ext/bridge/bridged_plugin.h"
#include "graph/graph_model.h"
#include "io/ftrk_reader.h"
#include "io/ftrk_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using nt::ext::bridge::LiveState;

// Point the session's bridge spawn at the built child binary (the same
// binary bridge_test uses). Without this the session resolves a sibling of
// the test executable, which is correct in an install layout but not the
// build tree.
void use_test_bridge_host() {
    ::setenv("NT_BRIDGE_HOST_EXE", NT_BRIDGE_HOST, /*overwrite=*/1);
}

// A bridged CLAP node, or empty when the environment can't spawn a child
// (headless CI without the shm/spawn support the bridge needs).
std::string add_bridged_sine(nt::app::ProjectSession& session) {
    if (!session.load_clap_file(NT_TEST_CLAP)) {
        return {};
    }
    const std::string ws = session.add_clap_node("nt.test.sine", /*bridged=*/true);
    if (ws.empty() || session.bridged_plugin(ws) == nullptr) {
        return {}; // spawn fell back to in-process: bridging unavailable here
    }
    return ws;
}

template <typename Predicate>
bool spin_wait(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(200us);
    }
    return predicate();
}

} // namespace

TEST_CASE("XPLG round-trips the bridged flag; absent reads as in-process", "[bridge][ftrk]") {
    nt::engine::TrackerProject project = nt::engine::create_project(2);

    nt::io::FtrkExternalPlugin in_process;
    in_process.workspace_id = "clap-1";
    in_process.kind = 0;
    in_process.plugin_id = "nt.test.sine";
    in_process.library_path = "/tmp/a.clap";
    in_process.state = {0x01, 0x02};
    in_process.bridged = false;

    nt::io::FtrkExternalPlugin bridged;
    bridged.workspace_id = "clap-2";
    bridged.kind = 0;
    bridged.plugin_id = "nt.test.crash";
    bridged.library_path = "/tmp/b.clap";
    bridged.state = {0x03, 0x04, 0x05};
    bridged.bridged = true;

    SECTION("a bridged record round-trips true") {
        nt::io::FtrkWriteExtras extras;
        extras.external = {in_process, bridged};
        const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, extras);
        std::string error;
        const auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
        REQUIRE(result.has_value());
        REQUIRE(result->extras.external.size() == 2);
        CHECK_FALSE(result->extras.external[0].bridged);
        CHECK(result->extras.external[1].bridged);
        // The rest of the record is unchanged by the additive tail.
        CHECK(result->extras.external[1].plugin_id == "nt.test.crash");
        CHECK(result->extras.external[1].state == std::vector<std::uint8_t>({0x03, 0x04, 0x05}));
    }

    SECTION("a file with no bridged plugin writes no tail and reads back false") {
        // The whole point of the trailing run: a project with nothing bridged
        // produces the pre-S29e XPLG byte layout, so old readers are unaffected
        // and new readers default the flag to false.
        nt::io::FtrkWriteExtras none_bridged;
        none_bridged.external = {in_process}; // bridged == false
        const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, none_bridged);

        nt::io::FtrkWriteExtras with_flag_all_false;
        auto also_false = in_process;
        with_flag_all_false.external = {also_false};
        // Writing with the flag defaulted false must be byte-identical to a
        // pre-S29e file (no trailing run emitted).
        CHECK(nt::io::write_ftrk(project, with_flag_all_false) == bytes);

        std::string error;
        const auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
        REQUIRE(result.has_value());
        REQUIRE(result->extras.external.size() == 1);
        CHECK_FALSE(result->extras.external[0].bridged);
    }
}

TEST_CASE("an old external record loads in-process, never bridged", "[bridge][session]") {
    // Back-compat at the session level: a project whose XPLG predates the
    // bridged flag (bridged=false) restores as an in-process CLAP node. Needs
    // only the in-process host, so it runs headlessly.
    nt::engine::TrackerProject project = nt::engine::create_project(2);
    nt::io::FtrkExternalPlugin external;
    external.workspace_id = "clap-1";
    external.kind = 0;
    external.plugin_id = "nt.test.sine";
    external.library_path = NT_TEST_CLAP;
    external.bridged = false;
    nt::io::FtrkWriteExtras extras;
    extras.external = {external};

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "nt_bridge_backcompat.ftrk";
    {
        const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, extras);
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), // NOLINT
                   static_cast<std::streamsize>(bytes.size()));
    }

    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_ftrk(path));

    // Exactly one external node, hosted in-process.
    std::string clap_ws;
    for (const nt::graph::Node& node : session.workspace().nodes()) {
        if (node.plugin_id.rfind("clap:", 0) == 0) {
            clap_ws = node.workspace_id;
        }
    }
    REQUIRE_FALSE(clap_ws.empty());
    CHECK(session.clap_instance(clap_ws) != nullptr);
    CHECK(session.bridged_plugin(clap_ws) == nullptr);
    CHECK_FALSE(session.external_plugin_bridged(clap_ws));
    std::filesystem::remove(path);
}

TEST_CASE("a bridged CLAP node binds and produces audio through the graph", "[bridge][session]") {
    use_test_bridge_host();
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);

    const std::string ws = add_bridged_sine(session);
    if (ws.empty()) {
        WARN("bridge spawn unavailable in this environment");
        SKIP("bridged CLAP node could not be created");
    }

    // The node hosts out-of-process: a BridgedPlugin binding, no in-process
    // ClapPlugin.
    CHECK(session.external_plugin_bridged(ws));
    CHECK(session.clap_instance(ws) == nullptr);
    CHECK(session.bridged_plugin(ws)->live_state() == LiveState::kLive);

    // Route the node's audio to the master and its notes through instrument
    // slot 1, then hold a note and pump the offline engine.
    REQUIRE(session.add_cable({.node_id = ws, .port_id = "main"},
                              {.node_id = nt::graph::kMasterInId, .port_id = "main"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);
    nt::engine::InstrumentTableEntry entry;
    entry.type = nt::engine::InstrumentSourceType::kWorkspace;
    entry.workspace_id = ws;
    session.set_instrument_entry(1, entry);
    session.preview_plugin_note(1, 69, 1.0F); // A-4, sustained

    float peak = 0.0F;
    std::array<float, 128 * 2> block{};
    for (int b = 0; b < 600; ++b) {
        audio.render_offline(block.data(), 128);
        for (const float v : block) {
            peak = std::max(peak, std::abs(v));
        }
        // Keep the child fed so the pipeline stays primed under test load.
        if (nt::ext::bridge::BridgedPlugin* bp = session.bridged_plugin(ws)) {
            spin_wait([&] { return bp->output_pending(); }, 20ms);
        }
    }
    CHECK(peak > 0.01F); // the bridged sine reached the master mix
    audio.stop();
}

TEST_CASE("toggling a bridged CLAP node re-creates it and carries state", "[bridge][session]") {
    use_test_bridge_host();
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);

    // Start bridged with a non-default gain (fixture param 0), then toggle
    // twice, asserting the hosting mode flips and the state survives.
    if (!session.load_clap_file(NT_TEST_CRASH_CLAP)) {
        SKIP("crash fixture unavailable");
    }
    const std::string ws = session.add_clap_node("nt.test.crash", /*bridged=*/true);
    if (ws.empty() || session.bridged_plugin(ws) == nullptr) {
        WARN("bridge spawn unavailable in this environment");
        SKIP("bridged CLAP node could not be created");
    }

    // Drive the gain param through the binding and let the child apply it,
    // so the shadow the toggle carries holds 0.5.
    std::array<float, 128 * 2> block{};
    session.bridged_plugin(ws)->plugin_set_param_cv(0, 0.5F);
    for (int b = 0; b < 8; ++b) {
        session.bridged_plugin(ws)->process_block(block.data(), block.data(), 128);
        REQUIRE(spin_wait([&] { return session.bridged_plugin(ws)->heartbeat() > 0; }, 500ms));
    }

    const auto gain_of = [](const std::vector<std::uint8_t>& blob) {
        double g = -1.0;
        if (blob.size() == sizeof(double)) {
            std::memcpy(&g, blob.data(), sizeof(double));
        }
        return g;
    };
    CHECK(std::abs(gain_of(session.bridged_plugin(ws)->save_state()) - 0.5) < 1e-6);

    // Bridged -> in-process: the binding disappears, an in-process instance
    // appears, and its state matches the carried shadow.
    REQUIRE(session.set_external_plugin_bridged(ws, false));
    CHECK(session.bridged_plugin(ws) == nullptr);
    REQUIRE(session.clap_instance(ws) != nullptr);
    CHECK_FALSE(session.external_plugin_bridged(ws));
    CHECK(std::abs(gain_of(session.clap_instance(ws)->save_state()) - 0.5) < 1e-6);

    // In-process -> bridged again: back to a live child carrying the state.
    REQUIRE(session.set_external_plugin_bridged(ws, true));
    REQUIRE(session.bridged_plugin(ws) != nullptr);
    CHECK(session.clap_instance(ws) == nullptr);
    CHECK(session.external_plugin_bridged(ws));
    CHECK(std::abs(gain_of(session.bridged_plugin(ws)->save_state()) - 0.5) < 1e-6);
    session.sweep_retired(); // let the superseded bindings reclaim
}

TEST_CASE("session-level crash -> badge state -> restart recovers", "[bridge][session]") {
    use_test_bridge_host();
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    if (!session.load_clap_file(NT_TEST_CRASH_CLAP)) {
        SKIP("crash fixture unavailable");
    }
    const std::string ws = session.add_clap_node("nt.test.crash", /*bridged=*/true);
    if (ws.empty() || session.bridged_plugin(ws) == nullptr) {
        WARN("bridge spawn unavailable in this environment");
        SKIP("bridged CLAP node could not be created");
    }
    nt::ext::bridge::BridgedPlugin* binding = session.bridged_plugin(ws);

    std::array<float, 128 * 2> block{};
    block.fill(0.3F);
    for (int b = 0; b < 8; ++b) {
        binding->process_block(block.data(), block.data(), 128);
        REQUIRE(spin_wait([&] { return binding->output_pending(); }, 500ms));
    }
    CHECK(binding->live_state() == LiveState::kLive);

    // Arm the crash (fixture param 1 > 0.5) and drive on so the child dies.
    binding->plugin_set_param_cv(1, 1.0F);
    for (int b = 0; b < 8; ++b) {
        binding->process_block(block.data(), block.data(), 128);
    }
    // The audio thread keeps returning promptly on the dead child.
    for (int b = 0; b < 256; ++b) {
        binding->process_block(block.data(), block.data(), 128);
    }

    // The reaper runs through the session's per-frame pump; within a bounded
    // number of frames the badge state latches crashed.
    bool crashed = false;
    for (int frame = 0; frame < 200 && !crashed; ++frame) {
        session.update_bridged();
        crashed = binding->live_state() == LiveState::kCrashed;
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(crashed);

    // The session stays saveable with the child dead (the shadow path).
    CHECK_FALSE(session.assemble_write_extras().external.empty());

    // One-click restart brings a fresh child back to live.
    REQUIRE(session.restart_bridged_plugin(ws));
    CHECK(binding->live_state() == LiveState::kLive);
    for (int b = 0; b < 8; ++b) {
        binding->process_block(block.data(), block.data(), 128);
        REQUIRE(spin_wait([&] { return binding->output_pending(); }, 500ms));
    }
}

TEST_CASE("a project saved with a bridged plugin reopens bridged", "[bridge][session]") {
    use_test_bridge_host();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "nt_bridge_persist.ftrk";

    {
        nt::audio::AudioEngine audio;
        nt::app::ProjectSession session(audio);
        const std::string ws = add_bridged_sine(session);
        if (ws.empty()) {
            WARN("bridge spawn unavailable in this environment");
            SKIP("bridged CLAP node could not be created");
        }
        REQUIRE(session.save_ftrk(path));
    }

    // The on-disk XPLG carries bridged=true.
    {
        std::string error;
        const auto result = nt::io::read_ftrk_file(path, error);
        REQUIRE(result.has_value());
        REQUIRE(result->extras.external.size() == 1);
        CHECK(result->extras.external[0].bridged);
    }

    // Reloading brings the node back bridged (a fresh child).
    {
        nt::audio::AudioEngine audio;
        nt::app::ProjectSession session(audio);
        REQUIRE(session.load_ftrk(path));
        std::string clap_ws;
        for (const nt::graph::Node& node : session.workspace().nodes()) {
            if (node.plugin_id.rfind("clap:", 0) == 0) {
                clap_ws = node.workspace_id;
            }
        }
        REQUIRE_FALSE(clap_ws.empty());
        CHECK(session.external_plugin_bridged(clap_ws));
        CHECK(session.bridged_plugin(clap_ws) != nullptr);
    }
    std::filesystem::remove(path);
}
