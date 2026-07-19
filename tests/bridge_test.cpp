// Out-of-process plugin bridge, S29a: the shared-memory transport and
// the echo/gain child, verified end to end against the real
// nanotracker-bridge-host binary (spawned via NT_BRIDGE_HOST).
//
// Coverage:
//   - shm index atomics are lock-free (the assumption the whole design
//     rests on, §A.5);
//   - a real child round-trips audio at exactly one block of latency
//     with the gain applied (§A.2/§A.6);
//   - killing the child mid-stream yields silence and never blocks or
//     crashes the host callback — the RT-safety core, run under ASan;
//   - process_block is allocation-free under an RtScope (the debug
//     allocator aborts on any allocation on the audio path);
//   - S29b: a child hosting the in-tree CLAP fixture produces audio that
//     matches the same fixture in-process (within float tolerance, after
//     the one-block latency); plugin state round-trips through the
//     control socket; a child that fails to load a plugin exits cleanly
//     and the host never hangs.
//
// Device-dependent bits (spawn + shm) WARN-skip when the environment
// cannot provide them; Linux CI supports both, so they run fully there.
#include "ext/bridge/bridge_protocol.h"
#include "ext/bridge/bridged_plugin.h"
#include "ext/clap_host.h"
#include "rt/rt_assert.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#endif

namespace {

using namespace std::chrono_literals;
using nt::ext::bridge::BridgedPlugin;

constexpr std::uint32_t kFrames = nt::ext::bridge::kBridgeBlockFrames;
constexpr std::uint32_t kStereo = kFrames * 2;

std::unique_ptr<BridgedPlugin> try_spawn(float gain, bool has_input, std::string& error) {
    BridgedPlugin::Config config;
    config.sample_rate = 48000;
    config.echo_gain = gain;
    config.has_audio_input = has_input;
    config.host_exe = NT_BRIDGE_HOST; // built child binary (CMake define)
    return BridgedPlugin::spawn(config, error);
}

// Spawn a child hosting the in-tree CLAP fixture (the same nt.test.sine
// clap_host_test drives), so the bridged output can be compared against
// the plugin in-process.
std::unique_ptr<BridgedPlugin> try_spawn_clap(const std::string& plugin_id, std::string& error) {
    BridgedPlugin::Config config;
    config.sample_rate = 48000;
    config.has_audio_input = false; // nt.test.sine is an instrument
    config.host_exe = NT_BRIDGE_HOST;
    config.plugin_path = NT_TEST_CLAP; // built fixture .clap (CMake define)
    config.plugin_id = plugin_id;
    return BridgedPlugin::spawn(config, error);
}

// Bounded spin on a predicate; returns its final value.
template <typename Predicate>
bool spin_wait(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(50us);
    }
    return predicate();
}

float peak(const std::array<float, kStereo>& block) {
    float p = 0.0F;
    for (const float v : block) {
        p = std::max(p, std::abs(v));
    }
    return p;
}

} // namespace

TEST_CASE("bridge shm index atomics are lock-free", "[bridge]") {
    // The cross-process release/acquire handshake only works if the
    // indices are genuinely lock-free (§A.4/§A.5).
    STATIC_CHECK(std::atomic<std::uint32_t>::is_always_lock_free);
    STATIC_CHECK(std::atomic<std::uint64_t>::is_always_lock_free);

    const auto control = std::make_unique<nt::ext::bridge::ControlBlock>();
    CHECK(control->in_write.is_lock_free());
    CHECK(control->out_write.is_lock_free());
    CHECK(control->child_heartbeat.is_lock_free());
}

TEST_CASE("bridge echo child round-trips audio at one block latency", "[bridge]") {
    constexpr float kGain = 0.5F;
    std::string error;
    auto plugin = try_spawn(kGain, /*has_input=*/true, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    constexpr int kBlocks = 16;
    std::vector<std::array<float, kStereo>> in(kBlocks);
    std::vector<std::array<float, kStereo>> out(kBlocks);
    for (int b = 0; b < kBlocks; ++b) {
        // A distinct constant per block so a misrouted block is caught.
        const float value = static_cast<float>(b + 1) * 0.01F;
        in[b].fill(value);
    }

    // Lockstep: push block b, then wait for the child to produce it, so
    // the pop on the *next* call returns exactly this block's echo. The
    // pipeline therefore reads back at exactly one block of latency.
    for (int b = 0; b < kBlocks; ++b) {
        plugin->process_block(in[b].data(), out[b].data(), kFrames);
        const bool produced = spin_wait([&] { return plugin->output_pending(); }, 500ms);
        REQUIRE(produced);
    }

    // out[0] is silence (nothing was ready at the first pop); out[b] for
    // b >= 1 is gain * in[b-1].
    CHECK(peak(out[0]) < 1e-6F);
    for (int b = 1; b < kBlocks; ++b) {
        const float expected = kGain * static_cast<float>(b) * 0.01F;
        INFO("block " << b);
        for (const float sample : out[b]) {
            REQUIRE(std::abs(sample - expected) < 1e-6F);
        }
    }
}

TEST_CASE("bridge host callback stays wait-free when the child is killed", "[bridge]") {
    std::string error;
    auto plugin = try_spawn(/*gain=*/1.0F, /*has_input=*/true, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    std::array<float, kStereo> input{};
    input.fill(0.25F);
    std::array<float, kStereo> output{};

    // Warm the pipeline and confirm audio is flowing.
    for (int b = 0; b < 4; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(spin_wait([&] { return plugin->output_pending(); }, 500ms));
    }
    plugin->process_block(input.data(), output.data(), kFrames);
    CHECK(peak(output) > 0.2F); // echo of the primed 0.25 input

#if defined(__linux__)
    REQUIRE(plugin->child_pid() > 0);
    REQUIRE(::kill(plugin->child_pid(), SIGKILL) == 0);
#endif

    // Drive the dead pipeline hard. Every call must return promptly (the
    // callback never waits on the corpse) and must never fault (ASan).
    for (int b = 0; b < 256; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process_block(input.data(), output.data(), kFrames);
        const auto dt = std::chrono::steady_clock::now() - t0;
        REQUIRE(dt < 50ms); // wait-free: no unbounded stall on a hung/dead child
    }

    // The output ring drains within a few blocks, after which the node
    // emits pure silence — the deadline-miss policy (§A.5).
    plugin->process_block(input.data(), output.data(), kFrames);
    CHECK(peak(output) < 1e-6F);
}

TEST_CASE("bridge process_block is allocation-free under RtScope", "[bridge]") {
    std::string error;
    auto plugin = try_spawn(/*gain=*/1.0F, /*has_input=*/false, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    std::array<float, kStereo> input{};
    input.fill(0.1F);
    std::array<float, kStereo> output{};

    // Under a debug build the global allocator aborts on any allocation
    // while this scope is live (rt/rt_assert.cpp); a release build makes
    // the scope a no-op, so the assertion has teeth only where it can.
    {
        [[maybe_unused]] const nt::rt::RtScope rt_scope;
        for (int b = 0; b < 64; ++b) {
            plugin->process_block(input.data(), output.data(), kFrames);
        }
    }
    SUCCEED("process_block ran allocation-free on the audio path");
}

TEST_CASE("bridged CLAP output matches the in-process plugin", "[bridge]") {
    // The headline correctness proof: the child hosting the fixture and
    // the same fixture in-process, given identical notes, must produce
    // identical audio — offset by exactly one block of pipeline latency.
    std::string error;
    auto bridged = try_spawn_clap("nt.test.sine", error);
    if (!bridged) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    // In-process reference of the same fixture.
    auto library = nt::ext::ClapLibrary::open(NT_TEST_CLAP, error);
    REQUIRE(library != nullptr);
    auto reference = nt::ext::ClapPlugin::create(*library, "nt.test.sine", 48000, error);
    INFO(error);
    REQUIRE(reference != nullptr);

    constexpr int kBlocks = 24;
    std::vector<std::array<float, kStereo>> ref_out(kBlocks);
    std::vector<std::array<float, kStereo>> br_out(kBlocks);

    // Hold A-4 on both from the first block.
    reference->plugin_note_on(69, 1.0F);
    bridged->plugin_note_on(69, 1.0F);
    for (int b = 0; b < kBlocks; ++b) {
        reference->process_block(nullptr, ref_out[b].data(), kFrames);
        bridged->process_block(nullptr, br_out[b].data(), kFrames);
        REQUIRE(spin_wait([&] { return bridged->output_pending(); }, 500ms));
    }

    // The reference actually makes sound (else the match is vacuous).
    CHECK(peak(ref_out[0]) > 0.1F);
    // One-block latency: br_out[0] is silence; br_out[b] == ref_out[b-1].
    CHECK(peak(br_out[0]) < 1e-6F);
    for (int b = 1; b < kBlocks; ++b) {
        INFO("block " << b);
        for (std::size_t i = 0; i < kStereo; ++i) {
            REQUIRE(std::abs(br_out[b][i] - ref_out[b - 1][i]) < 1e-5F);
        }
    }
}

TEST_CASE("bridge plugin state round-trips through the control socket", "[bridge]") {
    std::string error;
    auto first = try_spawn_clap("nt.test.sine", error);
    if (!first) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    // Drive the gain param (index 0, 0..1) to a known non-default value
    // and let the child apply it across a full pipeline round-trip before
    // asking it to save.
    std::array<float, kStereo> out{};
    first->plugin_set_param_cv(0, 0.8F);
    for (int b = 0; b < 4; ++b) {
        first->process_block(nullptr, out.data(), kFrames);
        REQUIRE(spin_wait([&] { return first->heartbeat() > 0; }, 500ms));
    }

    const std::vector<std::uint8_t> blob = first->save_state();
    // The fixture's state is its gain as a raw double.
    REQUIRE(blob.size() == sizeof(double));
    double saved_gain = 0.0;
    std::memcpy(&saved_gain, blob.data(), sizeof(double));
    CHECK(std::abs(saved_gain - 0.8) < 1e-6);

    // A fresh child, loaded with the blob, must re-serialise it identically.
    auto second = try_spawn_clap("nt.test.sine", error);
    REQUIRE(second != nullptr);
    REQUIRE(second->load_state(blob));
    const std::vector<std::uint8_t> reloaded = second->save_state();
    CHECK(reloaded == blob);
}

TEST_CASE("bridge child that fails to load a plugin exits without hanging", "[bridge]") {
    // A valid .clap but a wrong plugin id: the child's create() fails, so
    // it exits without signalling ready. The host must observe not-ready
    // and return null promptly — the graph then binds nothing and the
    // node falls back to silence (the runner's no-binding path).
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    auto plugin = try_spawn_clap("nt.does.not.exist", error);
    const auto dt = std::chrono::steady_clock::now() - t0;

    if (plugin) {
        // Spawn itself is unavailable here only if the child binary/shm is
        // missing; a successful spawn with a bad id is a real failure.
        FAIL("a bad plugin id must not produce a running bridge");
    }
    INFO(error);
    CHECK(dt < 3s); // bounded: no hang waiting on the failed child
    CHECK_FALSE(error.empty());
}
