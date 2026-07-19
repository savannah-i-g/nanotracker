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
//     allocator aborts on any allocation on the audio path).
//
// Device-dependent bits (spawn + shm) WARN-skip when the environment
// cannot provide them; Linux CI supports both, so they run fully there.
#include "ext/bridge/bridge_protocol.h"
#include "ext/bridge/bridged_plugin.h"
#include "rt/rt_assert.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
