// app/project_session — the open project and everything owned on its
// behalf: decoded sample buffers, the audio playback bundle, transport
// control, and the undo-routed edit API. This is the deliberate
// decomposition of the web app's Tracker.tsx god component: UI views
// render from here and mutate through here, never directly.
//
// Threading contract: the audio thread reads the project through the
// bundle pointer. Cell edits write fixed-size fields in preallocated
// rows (no reallocation), which is safe against the concurrent reader.
// Structural edits (pattern add/remove, sample replace) stop the
// transport first and republish the bundle; superseded buffers,
// bundles, racks and runners retire behind the generation fence in
// audio/sample_reclaim.h and free once the engine provably lets go.
#pragma once

#include "app/undo.h"
#include "audio/audio_engine.h"
#include "audio/sample_reclaim.h"
#include "engine/tracker_types.h"
#include "ext/clap_host.h"
#include "ext/editor_window.h"
#include "ext/vst3_host.h"
#include "graph/graph_model.h"
#include "graph/graph_wpbr.h"
#include "io/export_render.h"
#include "plugins/plugin_registry.h"
#include "plugins/preset_bank.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nt::app {

class ProjectSession {
public:
    explicit ProjectSession(audio::AudioEngine& audio);
    ~ProjectSession();

    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;

    // Replaces the open project with a fresh default one.
    void new_project();

    // Loads a project file by extension: .ftrk natively, .mod via the
    // importer (further formats join as their importers land). Returns
    // false with `error()` set. Sample audio is decoded and the
    // playback bundle republished.
    bool load_file(const std::filesystem::path& path);
    bool load_ftrk(const std::filesystem::path& path);

    // Writes the project as .ftrk v14 (workspace, bundled NTP
    // archives, project presets, external plugin state; POVR passes
    // through from the loaded file). Returns false with error() set.
    bool save_ftrk(const std::filesystem::path& path);

    // Offline render of the current project through a fresh engine
    // (io/export_render.h). UI thread; blocks until encoded. The
    // options overload is the full surface; the format one keeps the
    // CLI's defaults.
    io::ExportResult export_current(const std::filesystem::path& path, io::ExportFormat format);
    io::ExportResult export_current(const std::filesystem::path& path,
                                    const io::ExportOptions& options);

    // Write-time extras exactly as save_ftrk persists them (workspace,
    // bundled NTP archives, presets, external plugin state, POVR).
    // Public so export UIs can snapshot value copies for a worker
    // thread without a temp-file round-trip.
    [[nodiscard]] io::FtrkWriteExtras assemble_write_extras();

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] const std::vector<std::string>& load_warnings() const { return load_warnings_; }

    [[nodiscard]] engine::TrackerProject& project() { return project_; }

    [[nodiscard]] const engine::TrackerProject& project() const { return project_; }

    [[nodiscard]] UndoStack& undo() { return undo_; }

    // Transport.
    void play();
    void stop();
    [[nodiscard]] bool playing() const;

    // Cell edit routed through undo. Fields outside the pattern/row/
    // channel range are ignored.
    void set_cell(int pattern_index, int row, int channel, const engine::TrackerCell& cell);

    // Preview a note on a channel outside the sequencer (step-entry
    // audition). Resolves the sample the same way playback would.
    void preview_note(int channel, int slot, int note);

    // Sample slot management. Loading/clearing are structural: the
    // transport stops and the bundle republishes. Returns false with
    // error() set on decode failure.
    bool load_sample_into_slot(int slot, const std::filesystem::path& path);
    void clear_slot(int slot);

    // Metadata-only sample edit (volume/pan/base note/finetune/loops/
    // category/name) routed through undo; audio data is untouched.
    void set_sample_meta(int slot, const engine::TrackerSample& meta);

    // Instrument table entry edit routed through undo. Slot is 1-based;
    // the table grows to include it.
    void set_instrument_entry(int slot, const engine::InstrumentTableEntry& entry);

    // FX mixer editing. Structural changes (channels/modules) republish
    // the bundle with a fresh rack; parameter changes write the project
    // (undoable) and reach the live rack via command.
    void add_fx_channel();
    void remove_fx_channel(int channel);
    void add_fx_module(int channel, const std::string& module_id);
    void remove_fx_module(int channel, int module_index);
    void set_fx_module_param(int channel, int module_index, int param_index, float value);
    void set_fx_strip(int channel, int volume, int pan, bool enabled);
    void set_fx_send(int channel, int tracker_channel, float amount);

    // Sample id → decoded buffer (null when missing/undecodable).
    [[nodiscard]] const audio::SampleBuffer* sample_buffer(int sample_id) const;

    // ── Destructive waveform ops ─────────────────────────────────────
    // Ranges are half-open [start, end) in frames of the RESIDENT
    // (device-rate) buffer; end clamps to the buffer length, so
    // (0, UINT32_MAX) means "whole sample". Each op computes a new
    // buffer, re-encodes original_data as PCM16 WAV at the device rate
    // and rebuilds the resident audio by decoding those exact bytes —
    // what plays IS what FTRK stores (quantisation happens once, per
    // op, as in the web editor's re-encode). The old buffer retires
    // behind the reclamation fence and a bounded sample-op undo entry
    // restores the exact previous buffer object + metadata.
    //
    // Loop points (source-rate frames in TrackerSample) are rescaled
    // into the new device-rate base by every op; trim additionally
    // shifts them by the cut and clamps into the kept region (cleared
    // when nothing overlaps). Length-preserving ops (silence, fades,
    // normalize, reverse, gain, DC removal) leave loop placement
    // untouched — reverse deliberately keeps the loop window rather
    // than mirroring it, matching the web editor.
    //
    // Returns false with error() set (empty slot, empty range, silent
    // normalize selection).
    bool sample_trim(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame);
    bool sample_silence(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame);
    bool sample_fade_in(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame,
                        io::FadeShape shape);
    bool sample_fade_out(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame,
                         io::FadeShape shape);
    // Scales so the range's peak lands on `peak_target` (0..1] — up or
    // down (the web editor refused peaks near full scale; FIXES.md).
    bool sample_normalize(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame,
                          float peak_target);
    bool sample_reverse(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame);
    bool sample_gain_db(int sample_id, std::uint32_t start_frame, std::uint32_t end_frame,
                        float gain_db);
    // Whole-sample only: subtracts each channel's mean.
    bool sample_remove_dc(int sample_id);

    // Frees retired audio objects the engine has provably released
    // (audio/sample_reclaim.h). Called once per frame from the main
    // loop; disarms permanently if the engine ever dropped a command
    // (delivery of the unlinking publish becomes unprovable).
    void sweep_retired();

    [[nodiscard]] const audio::SampleReclaimer& reclaimer() const { return reclaimer_; }

    // ── Workspace patch graph ────────────────────────────────────────
    // The UI reads the model directly (window placements are written
    // back by the workspace view each frame); every structural change
    // goes through the methods below so the audio thread gets a fresh
    // schedule. Cable edits swap live — patching never stops playback.
    [[nodiscard]] graph::WorkspaceGraph& workspace() { return workspace_; }

    [[nodiscard]] const graph::DormantEntries& workspace_dormant() const {
        return workspace_dormant_;
    }

    // Bumped when a load replaces window placements; the workspace view
    // reapplies positions when it observes a new value.
    [[nodiscard]] int workspace_layout_generation() const { return workspace_layout_generation_; }

    // Validated connect, routed through undo on success. Returns the
    // result for visible UI feedback (fix #13).
    graph::ConnectResult add_cable(const graph::CableEnd& source, const graph::CableEnd& dest,
                                   graph::CableMode mode);
    bool remove_cable(const std::string& cable_id);
    bool set_cable_mode(const std::string& cable_id, graph::CableMode mode);

    // User-creatable workspace nodes: utility sums and NTP plugin
    // instances. remove_workspace_node refuses built-ins.
    std::string add_sum_node();
    bool remove_workspace_node(const std::string& workspace_id);

    // ── NTP plugins ──────────────────────────────────────────────────
    [[nodiscard]] plugins::PluginRegistry& plugin_registry() { return registry_; }

    // Loads an archive into the catalogue (no workspace node yet).
    // Returns the plugin id, or empty with error()/load_warnings set.
    std::string load_plugin_file(const std::filesystem::path& path);

    // Creates a workspace node + live instance of a catalogued plugin.
    // Returns the new workspace id (empty = unknown plugin).
    std::string add_plugin_node(const std::string& plugin_id);

    // Instance behind a workspace node (null for non-plugin nodes).
    [[nodiscard]] plugins::NtpInstance* plugin_instance(const std::string& workspace_id) const;

    // Routes a plugin parameter through the instance (live) — the
    // manifest dot-path convention applies.
    void set_plugin_param(const std::string& workspace_id, const std::string& key,
                          float value) const;

    // Republishes the graph after a node strip edit (volume/pan/
    // bypass) — the runner snapshots those at build. Live swap.
    void publish_workspace_strips() { publish_graph(); }

    // Note preview for plugin instruments (audition path). Sends the
    // previous preview's note-off first; stop() releases everything.
    void preview_plugin_note(int slot, int midi_note, float velocity);
    void preview_plugin_release();

    // ── Sequence layers (piano roll) ─────────────────────────────────
    // Note edits reallocate the layer vectors, which the sequencer
    // scans concurrently — so they are structural: transport stops and
    // the bundle republishes (same contract as pattern add/remove).
    // Layer-instrument changes are scalar writes and stay live.
    void seq_add_note(int pattern_index, int channel, int layer, const engine::SequenceNote& note);
    void seq_remove_note(int pattern_index, int channel, int layer, int note_index);
    // Replaces a layer's note set in one stop→mutate→publish window
    // (the batch form the transform toolbox needs). Notes are
    // re-sorted here; callers may pass any order.
    void seq_replace_notes(int pattern_index, int channel, int layer,
                           std::vector<engine::SequenceNote> notes);
    void seq_set_layer_instrument(int pattern_index, int channel, int layer, int instrument);
    void seq_set_layer_enabled(int pattern_index, int channel, int layer, bool enabled);
    // The layer container for editing/rendering; created on demand
    // (structural when it grows). Null when out of range.
    engine::SequenceLayer* seq_layer(int pattern_index, int channel, int layer,
                                     bool create_if_missing);

    [[nodiscard]] plugins::PresetBank& preset_bank() { return preset_bank_; }

    // ── External plugins (CLAP; VST3 joins with the same node shape) ─
    // Opens a .clap library into the catalogue. Returns false with
    // error() set.
    bool load_clap_file(const std::filesystem::path& path);

    struct ClapCatalogEntry {
        const ext::ClapLibrary* library = nullptr;
        ext::ClapLibrary::Descriptor descriptor;
    };

    [[nodiscard]] std::vector<ClapCatalogEntry> clap_catalog() const;

    // Instantiates a catalogued CLAP plugin as a workspace node.
    std::string add_clap_node(const std::string& plugin_id);

    // CLAP instance behind a workspace node (null for others).
    [[nodiscard]] ext::ClapPlugin* clap_instance(const std::string& workspace_id) const;

    // Editor OS windows. open returns false with error() set (no gui,
    // no display, refusal). update runs once per UI frame from the
    // main loop; closed windows tear down there.
    bool open_clap_editor(const std::string& workspace_id);
    [[nodiscard]] bool clap_editor_open(const std::string& workspace_id) const;
    void update_clap_editors();

    // VST3 — same node shape (see 08-external-plugins.md).
    bool load_vst3_file(const std::filesystem::path& path);

    struct Vst3CatalogEntry {
        ext::Vst3Module* module = nullptr;
        ext::Vst3Module::Descriptor descriptor;
    };

    [[nodiscard]] std::vector<Vst3CatalogEntry> vst3_catalog() const;
    std::string add_vst3_node(const std::string& class_id);
    [[nodiscard]] ext::Vst3Plugin* vst3_instance(const std::string& workspace_id) const;

private:
    void decode_samples();
    void publish_bundle();
    // Rebuilds the built-in nodes (bus sized to the project's channel
    // count) and clears cables/dormant state. Load and new-project only.
    void rebuild_workspace_nodes();
    // Live schedule republish via kSwapBundle (project/rack/samples
    // unchanged — the kSwapBundle sender contract).
    void publish_graph();
    // One structural slot swap: transport stops, the outgoing buffer
    // retires behind the publish fence, metadata replaces in place and
    // the bundle republishes. Shared ownership lets undo return the
    // exact original buffer object to the slot.
    void replace_sample_buffer(int sample_id, std::shared_ptr<audio::SampleBuffer> buffer,
                               const engine::TrackerSample& meta);
    // Common destructive-op scaffold: clamp range, run `edit` on a
    // copy of the resident audio, re-encode → re-decode, adjust loop
    // points, swap the slot and push the sample-op undo entry.
    bool
    apply_sample_edit(int sample_id, const char* label, std::uint32_t start_frame,
                      std::uint32_t end_frame, bool trim_to_range,
                      const std::function<std::vector<float>(const std::vector<float>&,
                                                             std::uint32_t, std::uint32_t)>& edit);
    // Creates the workspace node + instance for a catalogued plugin.
    std::string spawn_plugin_node(const std::string& plugin_id, const std::string& workspace_id);
    // Wakes dormant WPBR instruments whose plugins are catalogued, then
    // retries dormant cables against the now-live nodes.
    void wake_dormant_plugins();
    // Binds plugin instances into a freshly-built runner + bundle.
    void bind_plugins(audio::GraphRunner& runner, audio::PlaybackBundle& bundle);

    audio::AudioEngine& audio_;
    engine::TrackerProject project_;
    UndoStack undo_;

    // Decoded audio, indexed by sample id (shared: destructive-op undo
    // snapshots alias slot buffers; buffers are immutable once built).
    // Replaced slot buffers and superseded bundles/racks/runners
    // retire into the reclaimer and free once the engine provably
    // stops referencing them (audio/sample_reclaim.h fence).
    std::array<std::shared_ptr<audio::SampleBuffer>, engine::kMaxSamples + 1> buffers_{};
    std::unique_ptr<audio::PlaybackBundle> live_bundle_;
    std::unique_ptr<audio::FxRack> live_rack_;
    std::unique_ptr<audio::GraphRunner> live_runner_;
    audio::SampleReclaimer reclaimer_;
    // Monotone kSetBundle/kSwapBundle serial — the reclamation clock.
    std::uint64_t publish_serial_ = 0;

    graph::WorkspaceGraph workspace_;
    graph::DormantEntries workspace_dormant_;
    int workspace_layout_generation_ = 0;

    plugins::PluginRegistry registry_;
    plugins::PresetBank preset_bank_;
    std::map<std::string, plugins::NtpInstance*> instances_by_node_;
    std::vector<std::unique_ptr<ext::ClapLibrary>> clap_libraries_;
    std::vector<std::unique_ptr<ext::ClapPlugin>> clap_instances_;
    std::map<std::string, ext::ClapPlugin*> clap_by_node_;
    std::map<std::string, std::unique_ptr<ext::ClapEditorWindow>> clap_editors_;
    std::vector<std::unique_ptr<ext::Vst3Module>> vst3_modules_;
    std::vector<std::unique_ptr<ext::Vst3Plugin>> vst3_instances_;
    std::map<std::string, ext::Vst3Plugin*> vst3_by_node_;
    std::map<std::string, std::string> ext_path_by_node_; // library paths for XPLG
    std::vector<std::uint8_t> povr_raw_;                  // verbatim POVR round-trip
    int preview_plugin_slot_ = 0;
    int preview_plugin_note_ = -1;

    std::string error_;
    std::vector<std::string> load_warnings_;
};

} // namespace nt::app
