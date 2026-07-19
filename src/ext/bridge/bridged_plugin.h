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
// S29a scope: the child is a trivial echo/gain stand-in for a real
// plugin (bridge_host_main.cpp). Note / CV / transport marshalling,
// crash detection + restart, and the editor are later sub-stages; this
// binding carries only what the RT transport needs.
#pragma once

#include "audio/graph_runner.h"
#include "ext/bridge/bridge_shm.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace nt::ext::bridge {

class BridgedPlugin final : public audio::GraphPluginBinding {
public:
    struct Config {
        std::uint32_t sample_rate = 48000;
        // Effect (has dry input) vs instrument. Reserved for the S29c
        // bypass semantics (dry-passthrough vs silence); unused by the
        // S29a echo transport, which is symmetric either way.
        bool has_audio_input = false;
        // S29a echo transform: the child multiplies its input by this
        // gain — a stand-in for a real process() with trivially
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

    // S29a carries no events end to end; real marshalling into the ring
    // slots lands in S29b. Left as no-ops so the interface is satisfied.
    void plugin_note_on(int /*note*/, float /*velocity*/) override {}

    void plugin_note_off(int /*note*/) override {}

    // ── Off the audio thread (host session thread) ───────────────────
    // Wakes a parked child on the idle->active transition (§A.3). Never
    // called from process_block: waking is a syscall and the audio
    // thread must not issue one. Cheap when the child is already hot.
    void notify_active();

    // Test / diagnostic hooks (off the audio thread).
    [[nodiscard]] bool running() const;

    [[nodiscard]] int child_pid() const { return pid_; }

    [[nodiscard]] std::uint64_t heartbeat() const;
    [[nodiscard]] bool output_pending() const;

private:
    BridgedPlugin(BridgeShm shm, int pid, const Config& config);

    void teardown() noexcept;

    BridgeShm shm_;
    int pid_ = -1;
    std::uint64_t seq_ = 0;
    bool has_audio_input_ = false;
};

} // namespace nt::ext::bridge
