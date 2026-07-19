// ext/bridge/bridged_plugin — the host-side GraphPluginBinding that
// drives a plugin hosted in a child process over the shared-memory ring.
//
// From the graph's point of view a BridgedPlugin is just another
// GraphPluginBinding (audio/graph_runner.h:81): the runner calls
// process_block synchronously each block, exactly as it does for an
// in-process NTP or CLAP instance. The difference is entirely behind the
// interface — process_block does nothing but wait-free ring traffic:
// publish this block's input, consume the child's output-from-one-block-
// earlier, or write silence when none is ready. It never allocates,
// enters the kernel, or waits, under any child state (§A.1).
//
// S29b scope: the child hosts a real CLAP (bridge_host_main.cpp). This
// binding now marshals the runner's per-block note/param events into the
// input slot it publishes (see the event methods below — still wait-free,
// just fixed-storage slot writes) and proxies plugin state save/load over
// the control socket, off the RT path. Crash detection + restart and the
// cross-process editor are later sub-stages. An empty Config::plugin_path
// keeps the S29a echo/gain transport stand-in, which the pure-transport
// tests still exercise.
#pragma once

#include "audio/graph_runner.h"
#include "ext/bridge/bridge_shm.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::ext::bridge {

class BridgedPlugin final : public audio::GraphPluginBinding {
public:
    struct Config {
        std::uint32_t sample_rate = 48000;
        // Effect (has dry input) vs instrument. Reserved for the S29c
        // bypass semantics (dry-passthrough vs silence); the child also
        // uses it implicitly via the hosted plugin's own port layout.
        bool has_audio_input = false;
        // Real CLAP to host in the child. When plugin_path is set the
        // child opens the library and instantiates plugin_id; when it is
        // empty the child runs the S29a echo/gain transport stand-in
        // (echo_gain below), used by the pure-transport tests.
        std::filesystem::path plugin_path;
        std::string plugin_id;
        // Echo-mode transform (plugin_path empty): the child multiplies
        // its input by this gain — a process() stand-in with trivially
        // checkable output. Unity proves pure transport.
        float echo_gain = 1.0F;
        // Explicit bridge-host executable. Empty resolves it from
        // $NT_BRIDGE_HOST_EXE, then a sibling of this executable.
        std::filesystem::path host_exe;
    };

    // Spawns the child, maps the segment, and completes the version
    // handshake. Returns nullptr with `error` set if the bridge is
    // unavailable (spawn/shm failure, or a non-Linux platform) — the
    // caller then falls back to the runner's no-binding silence path.
    static std::unique_ptr<BridgedPlugin> spawn(const Config& config, std::string& error);

    ~BridgedPlugin() override;

    BridgedPlugin(const BridgedPlugin&) = delete;
    BridgedPlugin& operator=(const BridgedPlugin&) = delete;
    BridgedPlugin(BridgedPlugin&&) = delete;
    BridgedPlugin& operator=(BridgedPlugin&&) = delete;

    // ── GraphPluginBinding (audio thread, wait-free) ─────────────────
    void process_block(const float* in, float* out, std::uint32_t frames) override;

    // Note / CV events marshal straight into the block's input slot: each
    // appends one entry to the slot's fixed note/param arrays, published
    // by the next process_block. No allocation, no syscall — the slot is
    // preallocated shm the producer fills in place (§A.4). The arrays are
    // bounded (kBridgeMaxNotes/ParamsPerBlock); an overflow drops the
    // event and bumps a counter, never UB.
    void plugin_note_on(int note, float velocity) override;
    void plugin_note_off(int note) override;
    void plugin_set_param_cv(int param_index, float value01) override;
    void plugin_reset() override;

    // ── Off the audio thread (host session thread) ───────────────────
    // State proxy over the control socket (§C): save asks the child to
    // serialise its plugin and returns the blob; load ships a blob for the
    // child to apply. Blocking I/O — session thread only, never the audio
    // thread. Empty/false in echo mode or on any transport failure.
    [[nodiscard]] std::vector<std::uint8_t> save_state();
    bool load_state(const std::vector<std::uint8_t>& blob);

    // Wakes a parked child on the idle->active transition (§A.3). Never
    // called from process_block: waking is a syscall and the audio
    // thread must not issue one. Cheap when the child is already hot.
    void notify_active();

    // Test / diagnostic hooks (off the audio thread).
    [[nodiscard]] bool running() const;

    [[nodiscard]] int child_pid() const { return pid_; }

    [[nodiscard]] std::uint64_t heartbeat() const;
    [[nodiscard]] bool output_pending() const;

    // Dropped-event counters (overflow diagnostics; read off-thread).
    [[nodiscard]] std::uint64_t dropped_notes() const;
    [[nodiscard]] std::uint64_t dropped_params() const;
    [[nodiscard]] std::uint64_t dropped_input_blocks() const;

private:
    BridgedPlugin(BridgeShm shm, int pid, int control_fd, const Config& config);

    void teardown() noexcept;

    // Acquires (once per block) the input slot that plugin_note_on/off and
    // plugin_set_param_cv append into; process_block fills the rest and
    // commits it. Null when the ring is full (child behind) — events for
    // the block are then dropped, matching the empty-output silence policy.
    InSlot* open_input_slot();

    BridgeShm shm_;
    int pid_ = -1;
    int control_fd_ = -1; // host end of the state socketpair; -1 in echo mode
    std::uint64_t seq_ = 0;
    bool has_audio_input_ = false;

    // Current block's input slot, held across the runner's note/param
    // calls and the following process_block, then released on commit.
    InSlot* input_slot_ = nullptr;
    bool input_slot_open_ = false;
    bool pending_reset_ = false;

    // Written on the audio thread (relaxed), read off-thread for diagnostics.
    std::atomic<std::uint64_t> dropped_notes_{0};
    std::atomic<std::uint64_t> dropped_params_{0};
    std::atomic<std::uint64_t> dropped_input_blocks_{0};
};

} // namespace nt::ext::bridge
