// ext/clap_host — CLAP plugin hosting: library loading, the host
// callback surface, and instances that plug into the workspace graph
// through the same GraphPluginBinding NTP uses (external plugins are
// graph nodes like any other — 08-external-plugins.md).
//
// Threading follows the CLAP contract: library load, instantiation,
// activation, parameter description and state I/O happen on the main
// thread; process() and the event queues run on the audio thread.
// Parameter changes from the UI travel through a lock-free ring and
// are delivered as CLAP_EVENT_PARAM_VALUE at the head of the next
// block; note on/off arrive on the audio thread already (sequencer /
// preview commands) and are staged directly.
#pragma once

#include "audio/graph_runner.h"
#include "rt/spsc_queue.h"

#include <clap/clap.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::ext {

struct ClapParamInfo {
    clap_id id = 0;
    std::string name;
    std::string module;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
};

// One dlopen'd .clap file. Keeps the handle alive for the lifetime of
// every instance created from it (the registry retires, never unloads
// mid-session).
class ClapLibrary {
public:
    static std::unique_ptr<ClapLibrary> open(const std::filesystem::path& path, std::string& error);
    ~ClapLibrary();

    ClapLibrary(const ClapLibrary&) = delete;
    ClapLibrary& operator=(const ClapLibrary&) = delete;
    ClapLibrary(ClapLibrary&&) = delete;
    ClapLibrary& operator=(ClapLibrary&&) = delete;

    struct Descriptor {
        std::string id;
        std::string name;
        std::string vendor;
        std::string version;
        bool is_instrument = false;
    };

    [[nodiscard]] const std::vector<Descriptor>& descriptors() const { return descriptors_; }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] const clap_plugin_factory_t* factory() const { return factory_; }

private:
    ClapLibrary() = default;

    void* handle_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_factory_t* factory_ = nullptr;
    std::vector<Descriptor> descriptors_;
    std::filesystem::path path_;
};

// A live plugin instance, processing inside the workspace graph.
class ClapPlugin final : public audio::GraphPluginBinding {
public:
    // Instantiates + activates at `rate` with kGraphBlockFrames max
    // block size. Null with `error` set on any refusal.
    static std::unique_ptr<ClapPlugin> create(const ClapLibrary& library,
                                              const std::string& plugin_id, std::uint32_t rate,
                                              std::string& error);
    ~ClapPlugin() override;

    ClapPlugin(const ClapPlugin&) = delete;
    ClapPlugin& operator=(const ClapPlugin&) = delete;
    ClapPlugin(ClapPlugin&&) = delete;
    ClapPlugin& operator=(ClapPlugin&&) = delete;

    // ── GraphPluginBinding (audio thread) ────────────────────────────
    void process_block(const float* in, float* out, std::uint32_t frames) override;
    void plugin_note_on(int note, float velocity) override;
    void plugin_note_off(int note) override;
    // CV→param: param_index into params(); value01 maps into the
    // param's min..max and stages a CLAP_EVENT_PARAM_VALUE for the
    // next process_block (same staging the UI ring drains into;
    // bounded, drop-on-full). The main-thread param cache is left
    // alone — CV modulation is transient, like the web's CV →
    // AudioParam path which bypassed UI state.
    void plugin_set_param_cv(int param_index, float value01) override;

    // ── Main thread ──────────────────────────────────────────────────
    [[nodiscard]] const std::string& name() const { return name_; }

    [[nodiscard]] const std::string& plugin_id() const { return plugin_id_; }

    [[nodiscard]] bool has_audio_input() const { return has_audio_input_; }

    // True when the plugin exposes a note-in port (the session adds a
    // midi cable jack to the workspace node when it does).
    [[nodiscard]] bool has_note_input() const { return has_note_input_; }

    [[nodiscard]] const std::vector<ClapParamInfo>& params() const { return params_; }

    // Cached value (updated by set_param and state loads); the plugin
    // itself is only queried on the main thread.
    [[nodiscard]] double param_value(clap_id id) const;
    void set_param(clap_id id, double value); // → ring → next block

    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    bool load_state(const std::vector<std::uint8_t>& bytes);

    // The raw plugin, for the editor-window layer (gui extension).
    [[nodiscard]] const clap_plugin_t* raw() const { return plugin_; }

    // ── Host-extension plumbing (main thread) ────────────────────────
    // Backing for clap.posix-fd-support / clap.timer-support /
    // request_callback. pump_main_thread() runs once per UI frame
    // while an editor is open: polls registered fds, fires due timers,
    // services requested callbacks.
    bool host_register_fd(int fd, clap_posix_fd_flags_t flags);
    bool host_unregister_fd(int fd);
    bool host_register_timer(std::uint32_t period_ms, clap_id* timer_id);
    bool host_unregister_timer(clap_id timer_id);

    void host_request_callback_flag() { callback_requested_ = true; }

    void pump_main_thread();

private:
    ClapPlugin() = default;

    struct ParamChange {
        clap_id id = 0;
        double value = 0.0;
    };

    std::string name_;
    std::string plugin_id_;
    const clap_plugin_t* plugin_ = nullptr;
    clap_host_t host_{};
    std::uint32_t rate_ = 0;
    bool active_ = false;
    bool processing_ = false;
    bool has_audio_input_ = false;
    bool has_note_input_ = false;
    // Note-port dialect negotiation: some plugins (u-he among them)
    // only accept MIDI-dialect events on their note-in port.
    bool notes_as_midi_ = false;
    clap_id note_port_id_ = 0;
    std::vector<clap_event_midi_t> pending_midi_;

    std::vector<ClapParamInfo> params_;
    std::vector<std::pair<clap_id, double>> param_cache_;

    rt::SpscQueue<ParamChange> param_changes_{128};

    struct RegisteredFd {
        int fd = -1;
        clap_posix_fd_flags_t flags = 0;
    };

    struct RegisteredTimer {
        clap_id id = 0;
        std::uint32_t period_ms = 0;
        double next_due = 0.0; // steady-clock seconds
    };

    std::vector<RegisteredFd> fds_;
    std::vector<RegisteredTimer> timers_;
    clap_id next_timer_id_ = 1;
    bool callback_requested_ = false;

    // Audio-thread staging (preallocated; process() never allocates).
    std::vector<clap_event_note_t> pending_notes_;
    std::vector<clap_event_param_value_t> pending_params_;
    std::vector<float> planar_; // 4 channels × block frames
};

// Standard CLAP search paths (~/.clap, /usr/lib/clap) plus any extras.
[[nodiscard]] std::vector<std::filesystem::path> clap_search_paths();

} // namespace nt::ext
