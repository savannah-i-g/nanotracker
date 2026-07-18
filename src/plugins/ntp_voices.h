// plugins/ntp_voices — a playable NTP plugin instance: the manifest
// graph instantiated (shared nodes once, voice nodes per pool voice),
// the voice pool with stealing, per-block modulation, and the
// instrument/FX process entry points.
//
// Threading contract: build and parameter writes happen off the audio
// thread; process()/note_on()/note_off() run on it and never allocate.
// The session republishes instances RCU-style through the playback
// bundle, exactly like the FX rack and the workspace graph runner.
//
// Scope rules: sampler nodes are always shared — their polyphony is
// internal (zones/choke/round-robin state is instance-wide, matching
// the web's sampler node). Everything else honours the manifest scope.
//
// Modulation model: routes evaluate at block rate from the source's
// previous block (one-block mod latency — the price of arbitrary
// routing without a second scheduler), then slew toward the target.
// Audio-rate modulation uses param connections (`toParam`), which bind
// the live source block directly — that path has no added latency
// unless the edge closes a cycle.
#pragma once

#include "audio/graph_runner.h"
#include "plugins/ntp_graph.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nt::plugins {

// One user-assigned sample slot on a plugin instance: the decoded
// device-rate buffer the sampler plays plus everything the .ftrk POVR
// block persists (content hash, display name, source metadata, the
// original encoded bytes). Mirrors the web's SlotOverride +
// blob-store record collapsed into one per-instance entry — dedup by
// hash happens at serialisation time, not here.
struct SlotOverride {
    std::string hash; // "sha256:<hex>" of `bytes`
    std::string name; // original filename (display + POVR)
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 0;
    float duration = 0.0F;                       // seconds
    std::vector<std::uint8_t> bytes;             // encoded source file
    std::shared_ptr<audio::SampleBuffer> buffer; // device-rate decode
};

class NtpInstance final : public audio::GraphPluginBinding {
public:
    // Non-const plugin: set_env_stage writes the manifest's envelope
    // stage arrays in place (the one sanctioned mutation — everything
    // else reads).
    NtpInstance(LoadedNtpPlugin& plugin, std::uint32_t rate);

    NtpInstance(const NtpInstance&) = delete;
    NtpInstance& operator=(const NtpInstance&) = delete;
    NtpInstance(NtpInstance&&) = delete;
    NtpInstance& operator=(NtpInstance&&) = delete;
    ~NtpInstance() override = default;

    // GraphPluginBinding (workspace runner + sequencer note routing).
    void process_block(const float* in, float* out, std::uint32_t frames) override {
        process(in, out, frames);
    }

    void plugin_note_on(int note, float velocity) override { note_on(note, velocity); }

    void plugin_note_off(int note) override { note_off(note); }

    // Audio thread (kSetBundle drain): deactivates the voice pool and
    // every sampler voice so no SampleBuffer* survives the publish —
    // the fence that lets the session retire replaced slot-override
    // buffers (audio/sample_reclaim.h).
    void plugin_reset() override;

    // CV→param (audio thread): param_index into manifest().params;
    // value01 maps into the param's min..max. Writes the pre-resolved
    // node ParamSlot bases directly (no string work, no allocation)
    // and the host param value, so bare-key "param:" mod routes see
    // the motion too. Racing UI writes to the same floats are benign —
    // last writer wins, same as live set_param during playback.
    void plugin_set_param_cv(int param_index, float value01) override;

    [[nodiscard]] const ntp::Manifest& manifest() const { return plugin_->manifest; }

    // ── Audio thread ─────────────────────────────────────────────────
    // FX: `in` is the summed input block; instruments ignore it.
    // Output is written (not summed) into `out`, stereo interleaved.
    void process(const float* in, float* out, std::uint32_t frames);

    void note_on(int note, float velocity);
    void note_off(int note);
    void all_notes_off();

    // ── UI / session thread ──────────────────────────────────────────
    // Sets a host parameter. Dot-path keys ("nodeId.param") write the
    // node param base directly; bare keys only feed "param:" mod
    // routes. Values are stored for state snapshots either way.
    void set_param(const std::string& key, float value);
    [[nodiscard]] float param_value(const std::string& key) const;
    void apply_preset(const ntp::Preset& preset);

    // Peak of a node's last output block (meter controls).
    [[nodiscard]] float node_peak(const std::string& node_id) const;

    // Writes one envelope stage point on the owning plugin's manifest
    // node (the envelope-editor commit). Clamps to target 0..1 and
    // time ≥ 1 ms. Only inside the session's structural stop→publish
    // window: NodeRuntimes — across every instance of this plugin id,
    // the manifest is shared — read the stage array in place, so the
    // caller must fence the write exactly like a sequence-note edit
    // (ProjectSession::set_plugin_env_stage). Returns false when the
    // node is not an envelope or the stage index is out of range.
    bool set_env_stage(const std::string& node_id, int stage_index, double target, double time);

    // ── User sample slots (session thread) ───────────────────────────
    // Installs/clears an override for `slot_id` across every sampler
    // node (the runtime reads the swap through per-zone atomics at
    // trigger time). Returns the replaced decode buffer — the caller
    // retires it behind the reclamation fence; nothing here frees
    // audio the render thread might still reach.
    std::shared_ptr<audio::SampleBuffer> set_slot_override(const std::string& slot_id,
                                                           SlotOverride entry);
    std::shared_ptr<audio::SampleBuffer> clear_slot_override(const std::string& slot_id);

    // slot id → override, for the picker UI and the POVR serialiser.
    [[nodiscard]] const std::map<std::string, SlotOverride>& slot_overrides() const {
        return slot_overrides_;
    }

private:
    struct NodeInstantiation {
        std::unique_ptr<NodeRuntime> runtime;
        std::vector<float> out;    // current block
        std::vector<float> prev;   // previous block (delayed edges, mod)
        std::vector<float> in_sum; // gathered input
        float peak = 0.0F;
    };

    struct GraphNode {
        int def_index = -1; // into manifest graph.nodes
        bool voice_scoped = false;
        // instantiations[0] = shared; voice-scoped: one per pool voice.
        std::vector<NodeInstantiation> slots;
    };

    // Pseudo endpoints: kSrcExternal is the FX "input" block (silence
    // for instrument voiceIn); kSrcVoiceMix is the summed voiceOut of
    // all active voices, readable by shared nodes.
    static constexpr int kSrcExternal = -1;
    static constexpr int kSrcVoiceMix = -3;
    static constexpr int kDstOutput = -1;
    static constexpr int kDstVoiceOut = -2;

    struct Edge {
        int src = kSrcExternal;     // node index or pseudo
        int dst = kDstOutput;       // node index or pseudo
        std::string dst_param_name; // non-empty = param connection
        bool delayed = false;
    };

    struct ModState {
        float current = 0.0F;
    };

    struct Voice {
        bool active = false;
        VoiceControls controls;
        int note = -1;
        std::uint64_t age = 0;
        double fade = 1.0;                // built-in gate-off fade when no envelopes
        std::vector<ModState> mod_states; // parallel to flattened mod targets
    };

    void build_topology();
    void process_voice_pass(int voice_index, std::uint32_t frames);
    void apply_mod_routes(int voice_index, std::uint32_t frames);
    [[nodiscard]] NodeInstantiation& slot_for(int node_index, int voice_index);
    [[nodiscard]] int node_index_by_id(const std::string& id) const;

    LoadedNtpPlugin* plugin_; // non-const for set_env_stage only
    std::uint32_t rate_;
    bool is_instrument_;

    std::vector<GraphNode> nodes_;
    std::vector<int> order_;  // topological, delayed edges ignored
    std::vector<Edge> edges_; // signal + param edges, resolved to indices

    // Flattened mod targets: (route, target) pairs resolved to node/
    // param indices at build.
    struct ModBinding {
        int source_node = -1; // -1 = control/param source
        enum class Source : std::uint8_t { kNode, kVelocity, kNote, kGate, kPitch, kParam };
        Source source = Source::kNode;
        int param_index = -1; // into host param values (kParam)
        int target_node = -1;
        int target_voice_scoped = 0;
        std::string target_param;
        ntp::ModTarget def;
        float slew_coeff = 0.0F; // 0 = instant
    };

    std::vector<ModBinding> mod_bindings_;
    std::vector<ModState> shared_mod_states_; // for shared-target bindings

    std::vector<Voice> voices_;
    std::uint64_t voice_clock_ = 0;
    bool has_voice_envelopes_ = false;

    std::vector<std::pair<std::string, float>> host_params_; // key → value

    // Session-thread override table; the audio thread only ever sees
    // the raw buffer pointers pushed into the sampler runtimes.
    std::map<std::string, SlotOverride> slot_overrides_;

    // Per host param: value range + the ParamSlot bases its dot-path
    // resolves to (empty for bare keys). Built at construction so
    // plugin_set_param_cv is pure pointer writes on the audio thread.
    struct CvParamTarget {
        float min = 0.0F;
        float max = 1.0F;
        std::vector<ParamSlot*> slots;
    };

    std::vector<CvParamTarget> cv_targets_; // parallel to host_params_
    std::vector<float> voice_mix_;          // voiceOut sum
    std::vector<float> zero_block_;
    const float* external_in_ = nullptr; // FX input block during process()
};

} // namespace nt::plugins
