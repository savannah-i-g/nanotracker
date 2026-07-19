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
//   - S29c (the stage exit criterion): a bridged plugin's deliberate crash
//     leaves the host callback wait-free (ASan-clean), flips the node to
//     bypass (dry passthrough for an effect, silence for an instrument),
//     is confirmed by the session-thread reaper within a bounded number of
//     frames, keeps the session saveable from the shadow with the child
//     dead, and is recovered by restart() bringing a fresh child back to
//     live with the shadow state restored.
//
// Device-dependent bits (spawn + shm) WARN-skip when the environment
// cannot provide them; Linux CI supports both, so they run fully there.
#include "ext/bridge/bridge_control_socket.h"
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
#include <sys/socket.h>
#include <unistd.h>
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

// Spawn a child hosting the deliberately-crashing effect fixture. The host
// classifies it as an effect (has_input) or instrument (!has_input) to
// exercise the bypass kind-split; the fixture itself is a stereo passthrough
// (out = in * gain) with a param-armed abort() in its process(). Param
// index 0 is "gain" (state), index 1 is the crash trigger.
std::unique_ptr<BridgedPlugin> try_spawn_crash(bool has_input, std::string& error) {
    BridgedPlugin::Config config;
    config.sample_rate = 48000;
    config.has_audio_input = has_input;
    config.host_exe = NT_BRIDGE_HOST;
    config.plugin_path = NT_TEST_CRASH_CLAP; // built crash fixture .clap
    config.plugin_id = "nt.test.crash";
    return BridgedPlugin::spawn(config, error);
}

// Drive the reaper until it confirms the child's death, bounded so a hang
// fails rather than spins forever. Returns the frame count it took (or the
// bound on timeout — the caller asserts the state). The per-frame wait is
// generous (not a tight 1 ms) so that under heavy parallel-test load — many
// ctest workers each spawning a child — the crash-fixture's starved RT thread
// still gets scheduled to hit its abort()ing block within the bound; a real
// hang, where the child never dies, still exhausts the bound and fails.
int reap_until_crashed(BridgedPlugin& plugin, int max_frames) {
    for (int frame = 0; frame < max_frames; ++frame) {
        if (plugin.poll_liveness() || plugin.live_state() == nt::ext::bridge::LiveState::kCrashed) {
            return frame;
        }
        std::this_thread::sleep_for(5ms);
    }
    return max_frames;
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

    // Once the output ring drains and the heartbeat-staleness threshold
    // trips, the node enters BYPASS. For an effect (has_audio_input) bypass
    // is dry passthrough (§C.1) — a crashed insert must not punch a hole —
    // so the output now equals the input. (Pre-S29c the dead pipeline
    // drained to silence; S29c bypass supersedes that for effects.) The
    // 256-block drive above is far past the 32-block threshold. Without a
    // reaper poll here, the state stays kBypassed, not kCrashed.
    plugin->process_block(input.data(), output.data(), kFrames);
    for (const float sample : output) {
        REQUIRE(std::abs(sample - 0.25F) < 1e-6F);
    }
    CHECK(plugin->live_state() == nt::ext::bridge::LiveState::kBypassed);
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

TEST_CASE("bridge survives a child crash: bypass, reaper, saveable, restart", "[bridge]") {
    // THE S29c EXIT CRITERION. A bridged effect crashes on command; assert
    // the host callback stays wait-free (ASan-clean), output becomes dry
    // passthrough, the reaper confirms death within a bounded number of
    // frames, the session stays saveable from the shadow with the child
    // dead, and restart() brings a fresh child back to live with the shadow
    // state restored.
    using nt::ext::bridge::LiveState;
    std::string error;
    auto plugin = try_spawn_crash(/*has_input=*/true, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    // A constant non-zero input so passthrough vs gained-output is
    // unmistakable regardless of the one-block pipeline latency.
    std::array<float, kStereo> input{};
    input.fill(0.3F);
    std::array<float, kStereo> output{};

    // Set a non-default gain (0.5) — restart must restore THIS, not the
    // plugin default (1.0). Warm the pipeline so the child applies it.
    plugin->plugin_set_param_cv(0, 0.5F); // index 0 = gain
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(spin_wait([&] { return plugin->output_pending(); }, 500ms));
    }
    plugin->process_block(input.data(), output.data(), kFrames);
    CHECK(peak(output) > 0.1F); // effect passes input*gain = 0.15 pre-crash
    CHECK(plugin->live_state() == LiveState::kLive);

    // Snapshot the shadow while the child is alive (the persistence path):
    // it must be the fixture's gain (0.5) as a raw double.
    const std::vector<std::uint8_t> shadow = plugin->save_state();
    REQUIRE(shadow.size() == sizeof(double));
    double shadow_gain = 0.0;
    std::memcpy(&shadow_gain, shadow.data(), sizeof(double));
    CHECK(std::abs(shadow_gain - 0.5) < 1e-6);

    // Trigger the crash (index 1 armed > 0.5); the child aborts on the
    // block it applies it. Drive on so the death manifests.
    plugin->plugin_set_param_cv(1, 1.0F);
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
    }

    // (a) The host callback stays wait-free on the dead child, no fault
    //     (this loop is the load-bearing assertion under ASan).
    for (int b = 0; b < 256; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(std::chrono::steady_clock::now() - t0 < 50ms);
    }

    // (b) Output is bypass = dry passthrough for an effect (out == in),
    //     driven by the audio thread's heartbeat-staleness check alone.
    plugin->process_block(input.data(), output.data(), kFrames);
    for (const float sample : output) {
        REQUIRE(std::abs(sample - 0.3F) < 1e-6F);
    }

    // (c) The reaper confirms death within a bounded number of frames and
    //     latches crashed (never on the audio thread).
    const int frames_to_confirm = reap_until_crashed(*plugin, /*max_frames=*/200);
    INFO("frames to confirm death: " << frames_to_confirm);
    REQUIRE(frames_to_confirm < 200);
    CHECK(plugin->live_state() == LiveState::kCrashed);

    // (d) The session stays saveable with the child dead: the state path
    //     returns the shadow (never a hang), matching the pre-crash capture.
    const std::vector<std::uint8_t> saved_dead = plugin->save_state();
    CHECK(saved_dead == shadow);

    // Callback still wait-free and bypassing after the reaper latched.
    for (int b = 0; b < 32; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        for (const float sample : output) {
            REQUIRE(std::abs(sample - 0.3F) < 1e-6F); // still dry passthrough
        }
    }

    // (e) Restart brings a fresh child back to live producing correct audio,
    //     with the SHADOW gain (0.5) restored — not the default (1.0).
    REQUIRE(plugin->restart(error));
    CHECK(plugin->live_state() == LiveState::kLive);
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(spin_wait([&] { return plugin->output_pending(); }, 500ms));
    }
    plugin->process_block(input.data(), output.data(), kFrames);
    for (const float sample : output) {
        REQUIRE(std::abs(sample - 0.15F) < 1e-5F); // in * shadow gain 0.5
    }
}

TEST_CASE("bridge crash bypass is silence for an instrument", "[bridge]") {
    // The kind-split's other half (§C.1): a node the host treats as an
    // instrument (no dry input) bypasses to SILENCE on a crash, even while a
    // non-zero input signal is still being fed to it.
    using nt::ext::bridge::LiveState;
    std::string error;
    auto plugin = try_spawn_crash(/*has_input=*/false, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }

    std::array<float, kStereo> input{};
    input.fill(0.4F);
    std::array<float, kStereo> output{};

    // Pre-crash the fixture passes input*gain (default 1.0), so audio flows
    // even though the host classifies the node as an instrument.
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(spin_wait([&] { return plugin->output_pending(); }, 500ms));
    }
    plugin->process_block(input.data(), output.data(), kFrames);
    CHECK(peak(output) > 0.1F);

    // Crash it and let the reaper confirm.
    plugin->plugin_set_param_cv(1, 1.0F);
    for (int b = 0; b < 72; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
    }
    REQUIRE(reap_until_crashed(*plugin, /*max_frames=*/200) < 200);

    // Bypass for an instrument is SILENCE — no dry passthrough — despite the
    // non-zero input still being fed.
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(peak(output) < 1e-6F);
    }
    CHECK(plugin->live_state() == LiveState::kCrashed);
}

TEST_CASE("bridge editor control messages round-trip (open, window-id, close)",
          "[bridge][editor]") {
    // The display-INDEPENDENT core of the S29d editor path: the control-socket
    // messages (open -> window-id -> close). A socketpair stands in for the
    // control channel and a worker plays the child, servicing the editor
    // messages with a MOCK window id (no real child, no X display). It proves
    // the ControlMsg editor kinds, the header framing, and the EditorWindowInfo
    // POD all agree on both ends — the wire contract the host and child share.
#if defined(__linux__)
    using nt::ext::bridge::ControlHeader;
    using nt::ext::bridge::ControlMsg;
    using nt::ext::bridge::EditorWindowInfo;
    using nt::ext::bridge::kControlMagic;

    std::array<int, 2> sv{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) == 0);

    constexpr std::uint64_t kMockWindow = 0x00ABCDEF12345678ULL;
    constexpr std::uint32_t kW = 480;
    constexpr std::uint32_t kH = 320;

    // Worker (the "child"): read one request, reply; read another, reply. All
    // Catch2 assertions stay on the main thread — the worker only records what
    // it saw into locals read after join (a happens-before sync point).
    std::uint32_t saw_open = 0;
    std::uint32_t saw_close = 0;
    bool worker_ok = true;
    std::thread worker([&] {
        ControlHeader req{};
        if (!nt::ext::bridge::control_read_full(sv[1], &req, sizeof(req))) {
            worker_ok = false;
            return;
        }
        saw_open = req.type;
        const EditorWindowInfo info{kMockWindow, kW, kH, 1};
        const ControlHeader reply{kControlMagic,
                                  static_cast<std::uint32_t>(ControlMsg::kOpenEditorReply),
                                  static_cast<std::uint32_t>(sizeof(info)), 0};
        worker_ok = nt::ext::bridge::control_write_full(sv[1], &reply, sizeof(reply)) &&
                    nt::ext::bridge::control_write_full(sv[1], &info, sizeof(info));
        if (!worker_ok) {
            return;
        }
        ControlHeader creq{};
        if (!nt::ext::bridge::control_read_full(sv[1], &creq, sizeof(creq))) {
            worker_ok = false;
            return;
        }
        saw_close = creq.type;
        const ControlHeader creply{kControlMagic,
                                   static_cast<std::uint32_t>(ControlMsg::kCloseEditorReply), 0, 0};
        worker_ok = nt::ext::bridge::control_write_full(sv[1], &creply, sizeof(creply));
        ::close(sv[1]);
    });

    // Host side: send open, read the window info back.
    const ControlHeader open_req{kControlMagic,
                                 static_cast<std::uint32_t>(ControlMsg::kOpenEditorRequest), 0, 0};
    REQUIRE(nt::ext::bridge::control_write_full(sv[0], &open_req, sizeof(open_req)));
    ControlHeader reply{};
    REQUIRE(nt::ext::bridge::control_read_full(sv[0], &reply, sizeof(reply)));
    CHECK(reply.type == static_cast<std::uint32_t>(ControlMsg::kOpenEditorReply));
    CHECK(reply.status == 0);
    REQUIRE(reply.length == sizeof(EditorWindowInfo));
    EditorWindowInfo got{};
    REQUIRE(nt::ext::bridge::control_read_full(sv[0], &got, sizeof(got)));
    CHECK(got.window_id == kMockWindow);
    CHECK(got.width == kW);
    CHECK(got.height == kH);
    CHECK(got.resizable == 1U);

    // Host side: send close, read the ack.
    const ControlHeader close_req{
        kControlMagic, static_cast<std::uint32_t>(ControlMsg::kCloseEditorRequest), 0, 0};
    REQUIRE(nt::ext::bridge::control_write_full(sv[0], &close_req, sizeof(close_req)));
    ControlHeader close_reply{};
    REQUIRE(nt::ext::bridge::control_read_full(sv[0], &close_reply, sizeof(close_reply)));
    CHECK(close_reply.type == static_cast<std::uint32_t>(ControlMsg::kCloseEditorReply));

    worker.join();
    ::close(sv[0]);

    CHECK(worker_ok);
    CHECK(saw_open == static_cast<std::uint32_t>(ControlMsg::kOpenEditorRequest));
    CHECK(saw_close == static_cast<std::uint32_t>(ControlMsg::kCloseEditorRequest));
#else
    SUCCEED("cross-process editor bridge is Linux-only (§H.2)");
#endif
}

TEST_CASE("bridged editor embeds and survives a child crash while open", "[bridge][editor]") {
    // The end-to-end §D exit check, display-gated: the child creates the plugin
    // editor on its UI thread and hands back its X11 window id; the host
    // reparents that foreign window into a host container. Then the child is
    // crashed WHILE the editor is open — the risky path — and the host must
    // survive (no X fatal, no hang) and tear the container down cleanly, then
    // restart and reopen. WARN-skips headlessly (no X display -> the child
    // cannot create the surface, open_editor fails), matching the CLAP-editor
    // verification's headless posture.
    using nt::ext::bridge::LiveState;
    std::string error;
    auto plugin = try_spawn_crash(/*has_input=*/true, error);
    if (!plugin) {
        WARN("bridge spawn unavailable: " + error);
        SKIP("bridge child could not be spawned in this environment");
    }
    if (!plugin->open_editor(error)) {
        WARN("cross-process editor unavailable (headless?): " + error);
        SKIP("no X display for the cross-process editor");
    }
    CHECK(plugin->editor_open());

    // Pump the host container a few frames — must never fault or hang.
    for (int f = 0; f < 8; ++f) {
        plugin->update_editor();
        std::this_thread::sleep_for(2ms);
    }
    CHECK(plugin->editor_open());

    // Warm the audio pipeline, then crash the child WHILE the editor is open.
    std::array<float, kStereo> input{};
    input.fill(0.3F);
    std::array<float, kStereo> output{};
    for (int b = 0; b < 4; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(spin_wait([&] { return plugin->output_pending(); }, 500ms));
    }
    plugin->plugin_set_param_cv(1, 1.0F); // arm the crash (index 1 > 0.5)
    for (int b = 0; b < 8; ++b) {
        plugin->process_block(input.data(), output.data(), kFrames);
    }

    // The host audio callback stays wait-free on the dead child (ASan-clean).
    for (int b = 0; b < 128; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        plugin->process_block(input.data(), output.data(), kFrames);
        REQUIRE(std::chrono::steady_clock::now() - t0 < 50ms);
    }

    // The reaper confirms death; THEN update_editor tears the container down
    // without an X fatal or a hang against the now-destroyed foreign window.
    REQUIRE(reap_until_crashed(*plugin, /*max_frames=*/200) < 200);
    CHECK(plugin->live_state() == LiveState::kCrashed);
    plugin->update_editor();            // <- the load-bearing survival step
    CHECK_FALSE(plugin->editor_open()); // container gone, editor marked closed

    // The host survived: restart brings a fresh child and the editor reopens.
    REQUIRE(plugin->restart(error));
    CHECK(plugin->live_state() == LiveState::kLive);
    REQUIRE(plugin->open_editor(error));
    CHECK(plugin->editor_open());
    for (int f = 0; f < 4; ++f) {
        plugin->update_editor();
        std::this_thread::sleep_for(2ms);
    }
    plugin->close_editor();
    CHECK_FALSE(plugin->editor_open());
}
