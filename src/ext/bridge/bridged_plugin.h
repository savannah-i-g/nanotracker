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
// binding marshals the runner's per-block note/param events into the input
// slot it publishes (see the event methods below — still wait-free, just
// fixed-storage slot writes) and proxies plugin state save/load over the
// control socket, off the RT path.
//
// S29c scope (crash → bypass → restart): the binding OUTLIVES the child.
// Three crash signals, none blocking the audio thread —
//   1. heartbeat staleness, read lock-free in process_block: the ONLY
//      liveness logic on the audio thread, a counter compare that flips
//      output to BYPASS when the child stops advancing its heartbeat;
//   2. a session-thread reaper (poll_liveness): waitpid confirms + reaps
//      death and latches the CRASHED state that drives the badge/restart;
//   3. control-socket EOF, corroborating on the session thread.
// Bypass output is wait-free and kind-split (§C.1): effects pass the dry
// input through, instruments go silent. restart() swaps only the child
// process — the shm mapping, graph binding, bundle, and reclamation fence
// are all untouched — and reloads a host-side shadow of the last-known-good
// state so a fresh child restores meaningful state without ever calling
// save_state on the corpse (the shadow also keeps the session saveable with
// the child dead). The cross-process editor is a later sub-stage. An empty
// Config::plugin_path keeps the S29a echo/gain transport stand-in, which the
// pure-transport tests still exercise.
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

// Node liveness the badge UI reads off-thread (S29d/e). The audio thread
// flips kLive<->kBypassed lock-free from the heartbeat check; the reaper
// latches kCrashed on confirmed death; only restart() clears it back to
// kLive. Ordered by severity so a UI can compare. Host-side only (never on
// the wire), so it takes the smallest base type that holds it.
enum class LiveState : std::uint8_t {
    kLive = 0,     // child healthy, producing output
    kBypassed = 1, // heartbeat stale — audio thread is bypassing (recoverable)
    kCrashed = 2,  // reaper-confirmed death — latched until restart()
};

class BridgedPlugin final : public audio::GraphPluginBinding {
public:
    struct Config {
        std::uint32_t sample_rate = 48000;
        // Effect (has dry input) vs instrument. Drives the S29c bypass
        // kind-split (§C.1): on a crash an effect passes its dry input
        // through while an instrument goes silent. The child also uses the
        // hosted plugin's own port layout implicitly.
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
    // State proxy over the control socket (§C), shadow-backed. save_state
    // refreshes the last-known-good shadow from the live child and always
    // returns the shadow — so it stays valid (never a hang) even with a
    // dead child, keeping the session saveable (§C.4). load_state ships a
    // blob for the child to apply and adopts it as the shadow. Blocking
    // I/O — session thread only, never the audio thread.
    [[nodiscard]] std::vector<std::uint8_t> save_state();
    bool load_state(const std::vector<std::uint8_t>& blob);

    // Refresh the shadow from the live child without returning it (§C.4
    // cadence: S29d/e calls this after an editor session closes and on an
    // idle-dirty timer). No-op with a dead child. Returns whether it
    // refreshed. Session thread; blocking control I/O.
    bool capture_shadow();

    // The last-known-good state the persistence layer reads and restart()
    // reloads (§C.4). Session thread.
    [[nodiscard]] const std::vector<std::uint8_t>& shadow_state() const { return shadow_state_; }

    // ── Crash detection + restart (§C, session thread) ───────────────
    // The badge/liveness readout the UI polls (never the RT path).
    [[nodiscard]] LiveState live_state() const { return state_.load(std::memory_order_acquire); }

    // Reaper — call once per app-loop frame. Confirms child death via
    // waitpid(WNOHANG) (reaps the corpse) with control-socket EOF as a
    // corroborating signal, and latches kCrashed. Returns true only on the
    // frame it first transitions to crashed. Never on the audio thread.
    bool poll_liveness();

    // One-click restart (§C.3): reap the corpse, sanitise + reset the
    // reused shm segment, respawn the child, reload the shadow state, and
    // return to kLive. Only the child process swaps — the shm mapping,
    // graph binding, bundle, and reclamation fence are untouched, so the
    // audio thread keeps calling this same binding throughout. Precondition
    // kCrashed; returns false with `error` set (stays crashed) on failure.
    bool restart(std::string& error);

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
    BridgedPlugin(BridgeShm shm, const Config& config);

    void teardown() noexcept;

    // Spawns (or respawns) the child against the already-mapped segment:
    // builds argv from config_, sets up the control socket in CLAP mode,
    // posix_spawns, and waits (bounded, off-RT) for child_ready. Sets pid_
    // and control_fd_. Shared by the initial spawn() and restart().
    bool spawn_child(std::string& error);

    // Re-initialise the reused control block in place for a fresh child:
    // re-write the version-first header and zero the ring indices +
    // liveness words the untrusted dead child may have scribbled. Safe
    // uncontended — the audio thread does not touch shm while kCrashed.
    void reset_control_block();

    // Bypass output stage (§C.1, wait-free): dry passthrough for an effect
    // (has_audio_input), silence for an instrument. One memcpy or one fill;
    // never touches the child.
    void write_bypass(const float* in, float* out, std::uint32_t nsamp) const;

    // Control-socket save/load exchange bodies (§C). Session thread,
    // blocking; false on a dead/absent child, never a hang.
    bool control_save(std::vector<std::uint8_t>& out);
    bool control_load(const std::vector<std::uint8_t>& blob);

    // Acquires (once per block) the input slot that plugin_note_on/off and
    // plugin_set_param_cv append into; process_block fills the rest and
    // commits it. Null when the ring is full (child behind) — events for
    // the block are then dropped, matching the empty-output silence policy.
    InSlot* open_input_slot();

    BridgeShm shm_;
    Config config_; // retained for restart (path/id/rate/host_exe/gain)
    int pid_ = -1;
    int control_fd_ = -1; // host end of the state socketpair; -1 in echo mode
    std::uint64_t seq_ = 0;
    bool has_audio_input_ = false;

    // Current block's input slot, held across the runner's note/param
    // calls and the following process_block, then released on commit.
    InSlot* input_slot_ = nullptr;
    bool input_slot_open_ = false;
    bool pending_reset_ = false;

    // Liveness state (§C.2). Read off-thread by the UI; written by the
    // audio thread (kLive<->kBypassed via CAS that never leaves kCrashed)
    // and the session thread (reaper ->kCrashed, restart ->kLive).
    std::atomic<LiveState> state_{LiveState::kLive};

    // Audio-thread-only heartbeat staleness tracking (§B.4 signal 1). Not
    // atomic: touched solely in process_block, and quiescent while kCrashed
    // (so restart resets them under the state handoff without a race).
    std::uint64_t last_heartbeat_ = 0;
    std::uint32_t stale_blocks_ = 0;

    // Last-known-good state shadow (§C.4). Session thread only.
    std::vector<std::uint8_t> shadow_state_;

    // Written on the audio thread (relaxed), read off-thread for diagnostics.
    std::atomic<std::uint64_t> dropped_notes_{0};
    std::atomic<std::uint64_t> dropped_params_{0};
    std::atomic<std::uint64_t> dropped_input_blocks_{0};
};

} // namespace nt::ext::bridge
