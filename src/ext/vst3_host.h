// ext/vst3_host — VST3 plugin hosting on the SDK's hosting-support
// classes (Module/PlugProvider/HostProcessData/EventList/
// ParameterChanges). Same node shape as CLAP: instances implement
// GraphPluginBinding and live in the workspace graph; parameters use
// VST3's normalized 0..1 domain; state is the component+controller
// stream pair. CLAP proved the plumbing; this reuses it
// (08-external-plugins.md).
#pragma once

#include "audio/graph_runner.h"
#include "rt/spsc_queue.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::ext {

struct Vst3ParamInfo {
    std::uint32_t id = 0;
    std::string title;
    double def_normalized = 0.0; // VST3 params are normalized 0..1
    bool automatable = false;
};

class Vst3Plugin;

// One loaded .vst3 module (bundle directory or single file).
class Vst3Module {
public:
    static std::unique_ptr<Vst3Module> open(const std::filesystem::path& path, std::string& error);
    ~Vst3Module();

    Vst3Module(const Vst3Module&) = delete;
    Vst3Module& operator=(const Vst3Module&) = delete;
    Vst3Module(Vst3Module&&) = delete;
    Vst3Module& operator=(Vst3Module&&) = delete;

    struct Descriptor {
        std::string id; // class UID string
        std::string name;
        std::string vendor;
        std::string category; // "Instrument|Synth" etc.
        bool is_instrument = false;
    };

    [[nodiscard]] const std::vector<Descriptor>& descriptors() const { return descriptors_; }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    std::unique_ptr<Vst3Plugin> create(const std::string& class_id, std::uint32_t rate,
                                       std::string& error);

private:
    Vst3Module() = default;

    struct Impl; // hides SDK types from this header
    std::unique_ptr<Impl> impl_;
    std::vector<Descriptor> descriptors_;
    std::filesystem::path path_;
};

class Vst3Plugin final : public audio::GraphPluginBinding {
public:
    ~Vst3Plugin() override;

    Vst3Plugin(const Vst3Plugin&) = delete;
    Vst3Plugin& operator=(const Vst3Plugin&) = delete;
    Vst3Plugin(Vst3Plugin&&) = delete;
    Vst3Plugin& operator=(Vst3Plugin&&) = delete;

    // ── GraphPluginBinding (audio thread) ────────────────────────────
    void process_block(const float* in, float* out, std::uint32_t frames) override;
    void plugin_note_on(int note, float velocity) override;
    void plugin_note_off(int note) override;

    // ── Main thread ──────────────────────────────────────────────────
    [[nodiscard]] const std::string& name() const { return name_; }

    [[nodiscard]] bool has_audio_input() const { return has_audio_input_; }

    [[nodiscard]] const std::vector<Vst3ParamInfo>& params() const { return params_; }

    [[nodiscard]] double param_value(std::uint32_t id) const; // normalized
    void set_param(std::uint32_t id, double normalized);

    // Component + controller state streams, concatenated with a small
    // length header (the FTRK external-plugin chunk format).
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    bool load_state(const std::vector<std::uint8_t>& bytes);

private:
    friend class Vst3Module;
    Vst3Plugin() = default;

    struct ParamChange {
        std::uint32_t id = 0;
        double value = 0.0;
    };

    struct NoteEvent {
        int note = 0;
        float velocity = 0.0F;
        bool on = false;
    };

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string name_;
    bool has_audio_input_ = false;
    std::vector<Vst3ParamInfo> params_;
    std::vector<std::pair<std::uint32_t, double>> param_cache_;
    rt::SpscQueue<ParamChange> param_changes_{128};
    std::vector<NoteEvent> pending_notes_; // audio-thread staging
};

// Standard VST3 locations (~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3).
[[nodiscard]] std::vector<std::filesystem::path> vst3_search_paths();

} // namespace nt::ext
