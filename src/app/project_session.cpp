#include "app/project_session.h"

#include "engine/tracker_engine.h"
#include "graph/graph_compile.h"
#include "io/ftrk_reader.h"
#include "io/ftrk_writer.h"
#include "io/import/it_importer.h"
#include "io/import/mod_importer.h"
#include "io/import/s3m_importer.h"
#include "io/import/xm_importer.h"
#include "platform/paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <utility>

namespace nt::app {

ProjectSession::ProjectSession(audio::AudioEngine& audio)
    : audio_(audio), registry_(audio.sample_rate() != 0 ? audio.sample_rate() : 48000),
      preset_bank_(platform::config_dir() / "presets") {
    new_project();
}

ProjectSession::~ProjectSession() {
    stop();
}

void ProjectSession::new_project() {
    stop();
    project_ = engine::create_project(4);
    undo_.clear();
    for (auto& buffer : buffers_) {
        buffer.reset();
    }
    rebuild_workspace_nodes();
    publish_bundle();
}

bool ProjectSession::load_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool is_import = ext == ".mod" || ext == ".xm" || ext == ".s3m" || ext == ".it";
    if (!is_import) {
        return load_ftrk(path);
    }

    stop();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error_ = "cannot open " + path.string();
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    io::import::ImportResults results;
    std::optional<engine::TrackerProject> imported;
    if (ext == ".mod") {
        imported = io::import::import_mod(bytes.data(), bytes.size(), results);
    } else if (ext == ".xm") {
        imported = io::import::import_xm(bytes.data(), bytes.size(), results);
    } else if (ext == ".s3m") {
        imported = io::import::import_s3m(bytes.data(), bytes.size(), results);
    } else {
        imported = io::import::import_it(bytes.data(), bytes.size(), results);
    }
    load_warnings_ = std::move(results.warnings);
    load_warnings_.insert(load_warnings_.end(), results.info.begin(), results.info.end());
    if (!imported.has_value()) {
        error_ = results.errors.empty() ? "module import failed" : results.errors.front();
        return false;
    }
    project_ = std::move(*imported);
    undo_.clear();
    decode_samples();
    rebuild_workspace_nodes();
    publish_bundle();
    return true;
}

bool ProjectSession::load_ftrk(const std::filesystem::path& path) {
    stop();
    std::string error;
    auto result = io::read_ftrk_file(path, error);
    if (!result.has_value()) {
        error_ = error;
        return false;
    }
    project_ = std::move(result->project);
    // FXMX channel strips ride in extras; adopt them into the native
    // project model (color strings "#rrggbb" → packed RGB).
    project_.fx_mixer.channels.clear();
    for (const io::FtrkFxChannel& ch : result->extras.fx_channels) {
        engine::FxChannelStrip strip;
        strip.name = ch.name;
        strip.volume = ch.volume;
        strip.pan = ch.pan;
        strip.enabled = ch.enabled;
        strip.tracker_sends = ch.tracker_sends;
        if (ch.color.size() == 7 && ch.color[0] == '#') {
            strip.color = 0xFF000000U | static_cast<std::uint32_t>(
                                            std::strtoul(ch.color.c_str() + 1, nullptr, 16));
        }
        for (const io::FtrkFxModule& mod : ch.modules) {
            engine::FxModuleInstance instance;
            instance.module_id = mod.module_id;
            instance.enabled = mod.enabled;
            instance.params.assign(mod.params.begin(), mod.params.end());
            strip.modules.push_back(std::move(instance));
        }
        project_.fx_mixer.channels.push_back(std::move(strip));
    }
    load_warnings_ = std::move(result->extras.warnings);
    undo_.clear();
    decode_samples();
    rebuild_workspace_nodes();
    // Bundled plugin archives (PLGB) join the catalogue before the
    // patchbay adopts, so their instruments can wake immediately.
    for (const io::FtrkBundledPlugin& bundled : result->extras.plugins) {
        std::vector<std::string> plugin_errors;
        if (registry_.load_archive(bundled.archive.data(), bundled.archive.size(), plugin_errors)
                .empty()) {
            for (const std::string& e : plugin_errors) {
                load_warnings_.push_back("bundled plugin \"" + bundled.plugin_id + "\": " + e);
            }
        }
    }
    // WPBR patchbay state: window placements onto the built-in nodes,
    // cables through validated reconnect; plugin-era entries stay
    // dormant until their plugins (or stages) arrive.
    graph::WpbrAdoptResult adopted =
        graph::adopt_wpbr_json(workspace_, result->extras.workspace_json);
    workspace_dormant_ = std::move(adopted.dormant);
    load_warnings_.insert(load_warnings_.end(), adopted.warnings.begin(), adopted.warnings.end());
    wake_dormant_plugins();
    // Project-scope plugin presets (PPRS) attach to instances by
    // workspace id; unresolved entries stay stored for round-trip.
    preset_bank_.adopt_pprs(result->extras.pprs_json, load_warnings_);
    povr_raw_ = std::move(result->extras.povr_raw);
    // External plugin state (XPLG, v14): reopen libraries and restore.
    for (const io::FtrkExternalPlugin& external : result->extras.external) {
        std::string restore_error;
        bool restored = false;
        if (external.kind == 0) {
            if (load_clap_file(external.library_path)) {
                const std::string node_id = add_clap_node(external.plugin_id);
                if (!node_id.empty()) {
                    if (ext::ClapPlugin* instance = clap_instance(node_id)) {
                        instance->load_state(external.state);
                    }
                    restored = true;
                }
            }
        } else if (external.kind == 1) {
            if (load_vst3_file(external.library_path)) {
                const std::string node_id = add_vst3_node(external.plugin_id);
                if (!node_id.empty()) {
                    if (ext::Vst3Plugin* instance = vst3_instance(node_id)) {
                        instance->load_state(external.state);
                    }
                    restored = true;
                }
            }
        }
        if (!restored) {
            load_warnings_.push_back("external plugin \"" + external.plugin_id +
                                     "\" unavailable (" + external.library_path +
                                     ") — parameter snapshot kept in file only");
        }
    }
    publish_bundle();
    return true;
}

io::FtrkWriteExtras ProjectSession::assemble_write_extras() {
    io::FtrkWriteExtras extras;
    extras.workspace_json = graph::workspace_to_wpbr_json(workspace_, workspace_dormant_);
    extras.pprs_json = preset_bank_.to_pprs_json();
    extras.povr_raw = povr_raw_;
    // Bundle every NTP archive referenced by a workspace node (dedup).
    std::vector<std::string> bundled_ids;
    for (const auto& [workspace_id, instance] : instances_by_node_) {
        const std::string& plugin_id = instance->manifest().id;
        if (std::find(bundled_ids.begin(), bundled_ids.end(), plugin_id) != bundled_ids.end()) {
            continue;
        }
        if (const plugins::LoadedNtpPlugin* plugin = registry_.find(plugin_id)) {
            bundled_ids.push_back(plugin_id);
            extras.plugins.push_back({.plugin_id = plugin_id, .archive = plugin->archive_bytes});
        }
    }
    // External plugin state chunks + parameter snapshots.
    for (const auto& [workspace_id, instance] : clap_by_node_) {
        io::FtrkExternalPlugin external;
        external.workspace_id = workspace_id;
        external.kind = 0;
        external.plugin_id = instance->plugin_id();
        const auto path_it = ext_path_by_node_.find(workspace_id);
        external.library_path = path_it != ext_path_by_node_.end() ? path_it->second : "";
        external.state = instance->save_state();
        for (const ext::ClapParamInfo& param : instance->params()) {
            external.params.emplace_back(param.id, instance->param_value(param.id));
        }
        extras.external.push_back(std::move(external));
    }
    for (const auto& [workspace_id, instance] : vst3_by_node_) {
        io::FtrkExternalPlugin external;
        external.workspace_id = workspace_id;
        external.kind = 1;
        const graph::Node* node = workspace_.find_node(workspace_id);
        external.plugin_id =
            node != nullptr && node->plugin_id.size() > 5 ? node->plugin_id.substr(5) : "";
        const auto path_it = ext_path_by_node_.find(workspace_id);
        external.library_path = path_it != ext_path_by_node_.end() ? path_it->second : "";
        external.state = instance->save_state();
        for (const ext::Vst3ParamInfo& param : instance->params()) {
            external.params.emplace_back(param.id, instance->param_value(param.id));
        }
        extras.external.push_back(std::move(external));
    }
    return extras;
}

bool ProjectSession::save_ftrk(const std::filesystem::path& path) {
    std::string write_error;
    if (!io::write_ftrk_file(path, project_, assemble_write_extras(), write_error)) {
        error_ = write_error;
        return false;
    }
    return true;
}

io::ExportResult ProjectSession::export_current(const std::filesystem::path& path,
                                                io::ExportFormat format) {
    stop();
    return io::export_project(project_, assemble_write_extras(), path, format,
                              audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000);
}

void ProjectSession::decode_samples() {
    for (auto& buffer : buffers_) {
        buffer.reset();
    }
    for (const engine::TrackerSample& sample : project_.samples) {
        if (sample.id < 1 || sample.id > engine::kMaxSamples) {
            continue;
        }
        std::string decode_error;
        std::string format;
        auto buffer = audio::load_sample_memory(
            sample.original_data.data(), sample.original_data.size(),
            audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000, format, decode_error);
        if (buffer == nullptr) {
            load_warnings_.push_back("sample " + std::to_string(sample.id) + ": " + decode_error);
            continue;
        }
        buffer->name = sample.name;
        buffers_[static_cast<std::size_t>(sample.id)] = std::move(buffer);
    }
}

void ProjectSession::publish_bundle() {
    auto bundle = std::make_unique<audio::PlaybackBundle>();
    bundle->project = &project_;
    auto rack = std::make_unique<audio::FxRack>(
        project_.fx_mixer, audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000);
    bundle->fx_rack = rack.get();
    retired_racks_.push_back(std::move(rack));
    auto runner =
        std::make_unique<audio::GraphRunner>(workspace_, graph::compile_graph(workspace_));
    bind_plugins(*runner, *bundle);
    bundle->graph = runner.get();
    retired_runners_.push_back(std::move(runner));
    for (int id = 1; id <= engine::kMaxSamples; ++id) {
        bundle->samples_by_id[static_cast<std::size_t>(id)] =
            buffers_[static_cast<std::size_t>(id)].get();
    }
    live_bundle_ = bundle.get();
    retired_bundles_.push_back(std::move(bundle));
    audio_.send({.type = audio::Command::Type::kSetBundle,
                 .value = 0.0F,
                 .sample = nullptr,
                 .bundle = live_bundle_});
}

void ProjectSession::rebuild_workspace_nodes() {
    workspace_ = graph::WorkspaceGraph{};
    workspace_dormant_ = graph::DormantEntries{};
    instances_by_node_.clear(); // instances retire in the registry
    clap_by_node_.clear();      // instances retire in clap_instances_
    clap_editors_.clear();
    vst3_by_node_.clear();
    preview_plugin_note_ = -1;
    workspace_.add_node(graph::make_tracker_bus_node(project_.channels));
    workspace_.add_node(graph::make_master_in_node());
    workspace_.add_node(graph::make_module_player_node());
    ++workspace_layout_generation_;
}

void ProjectSession::publish_graph() {
    if (live_bundle_ == nullptr) {
        publish_bundle();
        return;
    }
    // Same project, rack and samples; only the schedule changes — so
    // the swap is live and FX tails/transport survive.
    auto runner =
        std::make_unique<audio::GraphRunner>(workspace_, graph::compile_graph(workspace_));
    auto bundle = std::make_unique<audio::PlaybackBundle>(*live_bundle_);
    bundle->plugin_by_slot.fill(nullptr);
    bind_plugins(*runner, *bundle);
    bundle->graph = runner.get();
    retired_runners_.push_back(std::move(runner));
    live_bundle_ = bundle.get();
    retired_bundles_.push_back(std::move(bundle));
    audio_.send({.type = audio::Command::Type::kSwapBundle,
                 .value = 0.0F,
                 .sample = nullptr,
                 .bundle = live_bundle_});
}

namespace {

graph::PortKind graph_kind_from(ntp::PortKind kind) {
    switch (kind) {
    case ntp::PortKind::kAudio:
        return graph::PortKind::kAudio;
    case ntp::PortKind::kSidechain:
        return graph::PortKind::kSidechain;
    case ntp::PortKind::kCv:
        return graph::PortKind::kCv;
    case ntp::PortKind::kGate:
        return graph::PortKind::kGate;
    case ntp::PortKind::kMidi:
        return graph::PortKind::kMidi;
    }
    return graph::PortKind::kAudio;
}

graph::Node make_plugin_graph_node(const ntp::Manifest& manifest, const std::string& workspace_id) {
    graph::Node node;
    node.workspace_id = workspace_id;
    node.plugin_id = manifest.id;
    node.display_name = manifest.name;
    node.kind = graph::NodeKind::kPlugin;
    node.window.x = 340.0F;
    node.window.y = 200.0F;
    for (const ntp::PortDef& port : manifest.inputs) {
        node.inputs.push_back(
            {.id = port.id, .label = port.label, .kind = graph_kind_from(port.kind)});
    }
    for (const ntp::PortDef& port : manifest.outputs) {
        node.outputs.push_back(
            {.id = port.id, .label = port.label, .kind = graph_kind_from(port.kind)});
    }
    return node;
}

} // namespace

std::string ProjectSession::load_plugin_file(const std::filesystem::path& path) {
    std::vector<std::string> errors;
    const std::string id = registry_.load_file(path, errors);
    if (id.empty()) {
        error_ = errors.empty() ? "plugin load failed" : errors.front();
        load_warnings_.insert(load_warnings_.end(), errors.begin(), errors.end());
    }
    return id;
}

std::string ProjectSession::spawn_plugin_node(const std::string& plugin_id,
                                              const std::string& workspace_id) {
    const plugins::LoadedNtpPlugin* plugin = registry_.find(plugin_id);
    if (plugin == nullptr) {
        return {};
    }
    plugins::NtpInstance* instance = registry_.create_instance(plugin_id);
    if (instance == nullptr) {
        return {};
    }
    workspace_.add_node(make_plugin_graph_node(plugin->manifest, workspace_id));
    instances_by_node_[workspace_id] = instance;
    for (const std::string& warning : plugin->warnings) {
        load_warnings_.push_back(plugin->manifest.name + ": " + warning);
    }
    return workspace_id;
}

std::string ProjectSession::add_plugin_node(const std::string& plugin_id) {
    std::string workspace_id = workspace_.next_node_id("plg");
    if (spawn_plugin_node(plugin_id, workspace_id).empty()) {
        return {};
    }
    // Node creation is structural for history (instances survive undo
    // by design — the registry owns them).
    undo_.clear();
    publish_graph();
    return workspace_id;
}

plugins::NtpInstance* ProjectSession::plugin_instance(const std::string& workspace_id) const {
    const auto it = instances_by_node_.find(workspace_id);
    return it != instances_by_node_.end() ? it->second : nullptr;
}

void ProjectSession::set_plugin_param(const std::string& workspace_id, const std::string& key,
                                      float value) const {
    if (plugins::NtpInstance* instance = plugin_instance(workspace_id)) {
        instance->set_param(key, value);
    }
}

void ProjectSession::preview_plugin_note(int slot, int midi_note, float velocity) {
    if (preview_plugin_note_ >= 0) {
        audio_.send({.type = audio::Command::Type::kPluginNoteOff,
                     .value = 0.0F,
                     .aux_int = (preview_plugin_slot_ << 8) | preview_plugin_note_});
    }
    audio_.send({.type = audio::Command::Type::kPluginNoteOn,
                 .value = velocity,
                 .aux_int = (slot << 8) | (midi_note & 0xFF)});
    preview_plugin_slot_ = slot;
    preview_plugin_note_ = midi_note & 0xFF;
}

void ProjectSession::preview_plugin_release() {
    if (preview_plugin_note_ >= 0) {
        audio_.send({.type = audio::Command::Type::kPluginNoteOff,
                     .value = 0.0F,
                     .aux_int = (preview_plugin_slot_ << 8) | preview_plugin_note_});
        preview_plugin_note_ = -1;
    }
}

engine::SequenceLayer* ProjectSession::seq_layer(int pattern_index, int channel, int layer,
                                                 bool create_if_missing) {
    if (pattern_index < 0 || pattern_index >= static_cast<int>(project_.patterns.size()) ||
        channel < 0 || channel >= project_.channels || layer < 0 ||
        layer >= engine::kMaxSeqLayersPerChannel) {
        return nullptr;
    }
    auto& seq_patterns = project_.sequence_mixer.seq_patterns;
    const bool needs_growth =
        pattern_index >= static_cast<int>(seq_patterns.size()) ||
        channel >=
            static_cast<int>(seq_patterns[static_cast<std::size_t>(pattern_index)].layers.size()) ||
        layer >= static_cast<int>(seq_patterns[static_cast<std::size_t>(pattern_index)]
                                      .layers[static_cast<std::size_t>(channel)]
                                      .size());
    if (needs_growth) {
        if (!create_if_missing) {
            return nullptr;
        }
        stop(); // container growth is structural
        if (pattern_index >= static_cast<int>(seq_patterns.size())) {
            seq_patterns.resize(project_.patterns.size());
        }
        auto& pattern = seq_patterns[static_cast<std::size_t>(pattern_index)];
        if (channel >= static_cast<int>(pattern.layers.size())) {
            pattern.layers.resize(static_cast<std::size_t>(project_.channels));
        }
        auto& layers = pattern.layers[static_cast<std::size_t>(channel)];
        if (layer >= static_cast<int>(layers.size())) {
            layers.resize(static_cast<std::size_t>(engine::kMaxSeqLayersPerChannel));
        }
        publish_bundle();
    }
    return &project_.sequence_mixer.seq_patterns[static_cast<std::size_t>(pattern_index)]
                .layers[static_cast<std::size_t>(channel)][static_cast<std::size_t>(layer)];
}

void ProjectSession::seq_add_note(int pattern_index, int channel, int layer,
                                  const engine::SequenceNote& note) {
    engine::SequenceLayer* target = seq_layer(pattern_index, channel, layer, true);
    if (target == nullptr) {
        return;
    }
    stop();
    // Keep the tick-sorted invariant the scanner relies on.
    auto at = std::upper_bound(target->notes.begin(), target->notes.end(), note,
                               [](const engine::SequenceNote& a, const engine::SequenceNote& b) {
                                   return a.start_tick < b.start_tick;
                               });
    target->notes.insert(at, note);
    undo_.clear();
    publish_bundle();
}

void ProjectSession::seq_remove_note(int pattern_index, int channel, int layer, int note_index) {
    engine::SequenceLayer* target = seq_layer(pattern_index, channel, layer, false);
    if (target == nullptr || note_index < 0 ||
        note_index >= static_cast<int>(target->notes.size())) {
        return;
    }
    stop();
    target->notes.erase(target->notes.begin() + note_index);
    undo_.clear();
    publish_bundle();
}

void ProjectSession::seq_set_layer_instrument(int pattern_index, int channel, int layer,
                                              int instrument) {
    engine::SequenceLayer* target = seq_layer(pattern_index, channel, layer, true);
    if (target != nullptr) {
        target->instrument = instrument; // scalar write — safe live
    }
}

void ProjectSession::seq_set_layer_enabled(int pattern_index, int channel, int layer,
                                           bool enabled) {
    engine::SequenceLayer* target = seq_layer(pattern_index, channel, layer, false);
    if (target != nullptr) {
        target->enabled = enabled;
    }
}

void ProjectSession::wake_dormant_plugins() {
    using nlohmann::json;
    std::vector<std::string> still_dormant;
    for (const std::string& raw : workspace_dormant_.instruments) {
        const json inst = json::parse(raw, nullptr, false);
        if (inst.is_discarded() || !inst.is_object()) {
            continue;
        }
        const std::string plugin_id = inst.value("pluginId", "");
        const std::string workspace_id = inst.value("workspaceId", "");
        if (registry_.find(plugin_id) == nullptr || workspace_id.empty() ||
            workspace_.find_node(workspace_id) != nullptr) {
            still_dormant.push_back(raw);
            continue;
        }
        if (spawn_plugin_node(plugin_id, workspace_id).empty()) {
            still_dormant.push_back(raw);
            continue;
        }
        graph::Node* node = workspace_.find_node(workspace_id);
        if (inst.contains("windowState") && inst["windowState"].is_object()) {
            const json& w = inst["windowState"];
            node->window.visible = w.value("visible", true);
            node->window.x = w.value("x", node->window.x);
            node->window.y = w.value("y", node->window.y);
            node->window.minimised = w.value("minimised", false);
            node->window.z_index = w.value("zIndex", 0);
        }
        node->volume = inst.value("volume", 1.0F);
        node->pan = inst.value("pan", 0.0F);
        node->bypass = inst.value("bypass", false);
        if (plugins::NtpInstance* instance = plugin_instance(workspace_id)) {
            // Bound json before .items() — the sub-expression temporary
            // would not outlive the loop in C++20.
            const json snapshot = inst.value("paramSnapshot", json::object());
            for (const auto& [key, value] : snapshot.items()) {
                if (value.is_number()) {
                    instance->set_param(key, value.get<float>());
                }
            }
        }
    }
    workspace_dormant_.instruments = std::move(still_dormant);

    // Retry dormant cables now that plugin nodes exist.
    if (!workspace_dormant_.cables.empty()) {
        json retry = json::object();
        retry["instruments"] = json::array();
        json cables = json::array();
        for (const std::string& raw : workspace_dormant_.cables) {
            json parsed = json::parse(raw, nullptr, false);
            if (!parsed.is_discarded()) {
                cables.push_back(std::move(parsed));
            }
        }
        retry["cables"] = std::move(cables);
        graph::WpbrAdoptResult readopted = graph::adopt_wpbr_json(workspace_, retry.dump());
        workspace_dormant_.cables = std::move(readopted.dormant.cables);
        load_warnings_.insert(load_warnings_.end(), readopted.warnings.begin(),
                              readopted.warnings.end());
    }
}

bool ProjectSession::load_clap_file(const std::filesystem::path& path) {
    std::string clap_error;
    auto library = ext::ClapLibrary::open(path, clap_error);
    if (library == nullptr) {
        error_ = path.filename().string() + ": " + clap_error;
        return false;
    }
    // Replace an already-open copy of the same file (retire the old
    // library — live instances keep their handle alive through it).
    clap_libraries_.push_back(std::move(library));
    return true;
}

std::vector<ProjectSession::ClapCatalogEntry> ProjectSession::clap_catalog() const {
    std::vector<ClapCatalogEntry> catalog;
    for (const auto& library : clap_libraries_) {
        for (const ext::ClapLibrary::Descriptor& descriptor : library->descriptors()) {
            catalog.push_back({.library = library.get(), .descriptor = descriptor});
        }
    }
    return catalog;
}

std::string ProjectSession::add_clap_node(const std::string& plugin_id) {
    for (const auto& library : clap_libraries_) {
        for (const ext::ClapLibrary::Descriptor& descriptor : library->descriptors()) {
            if (descriptor.id != plugin_id) {
                continue;
            }
            std::string clap_error;
            auto instance = ext::ClapPlugin::create(
                *library, plugin_id, audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000,
                clap_error);
            if (instance == nullptr) {
                error_ = clap_error;
                return {};
            }
            const std::string workspace_id = workspace_.next_node_id("clap");
            graph::Node node;
            node.workspace_id = workspace_id;
            node.plugin_id = "clap:" + plugin_id;
            node.display_name = instance->name();
            node.kind = graph::NodeKind::kPlugin;
            node.window.x = 380.0F;
            node.window.y = 240.0F;
            if (instance->has_audio_input()) {
                node.inputs.push_back({.id = "in", .label = "IN", .kind = graph::PortKind::kAudio});
            }
            node.outputs.push_back({.id = "main", .label = "OUT", .kind = graph::PortKind::kAudio});
            workspace_.add_node(std::move(node));
            clap_by_node_[workspace_id] = instance.get();
            ext_path_by_node_[workspace_id] = library->path().string();
            clap_instances_.push_back(std::move(instance));
            undo_.clear();
            publish_graph();
            return workspace_id;
        }
    }
    error_ = "unknown CLAP plugin \"" + plugin_id + "\"";
    return {};
}

ext::ClapPlugin* ProjectSession::clap_instance(const std::string& workspace_id) const {
    const auto it = clap_by_node_.find(workspace_id);
    return it != clap_by_node_.end() ? it->second : nullptr;
}

bool ProjectSession::open_clap_editor(const std::string& workspace_id) {
    if (clap_editors_.contains(workspace_id)) {
        return true;
    }
    ext::ClapPlugin* instance = clap_instance(workspace_id);
    if (instance == nullptr) {
        error_ = "no CLAP instance for \"" + workspace_id + "\"";
        return false;
    }
    std::string editor_error;
    auto editor = ext::ClapEditorWindow::open(*instance, editor_error);
    if (editor == nullptr) {
        error_ = editor_error;
        return false;
    }
    clap_editors_[workspace_id] = std::move(editor);
    return true;
}

bool ProjectSession::clap_editor_open(const std::string& workspace_id) const {
    return clap_editors_.contains(workspace_id);
}

void ProjectSession::update_clap_editors() {
    for (auto it = clap_editors_.begin(); it != clap_editors_.end();) {
        if (!it->second->update()) {
            it = clap_editors_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ProjectSession::load_vst3_file(const std::filesystem::path& path) {
    std::string vst3_error;
    auto module = ext::Vst3Module::open(path, vst3_error);
    if (module == nullptr) {
        error_ = path.filename().string() + ": " + vst3_error;
        return false;
    }
    vst3_modules_.push_back(std::move(module));
    return true;
}

std::vector<ProjectSession::Vst3CatalogEntry> ProjectSession::vst3_catalog() const {
    std::vector<Vst3CatalogEntry> catalog;
    for (const auto& module : vst3_modules_) {
        for (const ext::Vst3Module::Descriptor& descriptor : module->descriptors()) {
            catalog.push_back({.module = module.get(), .descriptor = descriptor});
        }
    }
    return catalog;
}

std::string ProjectSession::add_vst3_node(const std::string& class_id) {
    for (const auto& module : vst3_modules_) {
        for (const ext::Vst3Module::Descriptor& descriptor : module->descriptors()) {
            if (descriptor.id != class_id) {
                continue;
            }
            std::string vst3_error;
            auto instance = module->create(
                class_id, audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000, vst3_error);
            if (instance == nullptr) {
                error_ = vst3_error;
                return {};
            }
            const std::string workspace_id = workspace_.next_node_id("vst3");
            graph::Node node;
            node.workspace_id = workspace_id;
            node.plugin_id = "vst3:" + class_id;
            node.display_name = instance->name();
            node.kind = graph::NodeKind::kPlugin;
            node.window.x = 420.0F;
            node.window.y = 280.0F;
            if (instance->has_audio_input()) {
                node.inputs.push_back({.id = "in", .label = "IN", .kind = graph::PortKind::kAudio});
            }
            node.outputs.push_back({.id = "main", .label = "OUT", .kind = graph::PortKind::kAudio});
            workspace_.add_node(std::move(node));
            vst3_by_node_[workspace_id] = instance.get();
            ext_path_by_node_[workspace_id] = module->path().string();
            vst3_instances_.push_back(std::move(instance));
            undo_.clear();
            publish_graph();
            return workspace_id;
        }
    }
    error_ = "unknown VST3 class \"" + class_id + "\"";
    return {};
}

ext::Vst3Plugin* ProjectSession::vst3_instance(const std::string& workspace_id) const {
    const auto it = vst3_by_node_.find(workspace_id);
    return it != vst3_by_node_.end() ? it->second : nullptr;
}

void ProjectSession::bind_plugins(audio::GraphRunner& runner, audio::PlaybackBundle& bundle) {
    const std::vector<graph::Node>& nodes = workspace_.nodes();
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        if (nodes[n].kind != graph::NodeKind::kPlugin) {
            continue;
        }
        const auto it = instances_by_node_.find(nodes[n].workspace_id);
        if (it != instances_by_node_.end()) {
            runner.bind_plugin(static_cast<int>(n), it->second);
            continue;
        }
        const auto clap_it = clap_by_node_.find(nodes[n].workspace_id);
        if (clap_it != clap_by_node_.end()) {
            runner.bind_plugin(static_cast<int>(n), clap_it->second);
            continue;
        }
        const auto vst3_it = vst3_by_node_.find(nodes[n].workspace_id);
        if (vst3_it != vst3_by_node_.end()) {
            runner.bind_plugin(static_cast<int>(n), vst3_it->second);
        }
    }
    // Instrument-table plugin slots → bindings for the sequencer.
    for (std::size_t i = 0; i < project_.instrument_table.size() && i < engine::kMaxSamples; ++i) {
        const engine::InstrumentTableEntry& entry = project_.instrument_table[i];
        if (entry.type != engine::InstrumentSourceType::kPlugin &&
            entry.type != engine::InstrumentSourceType::kWorkspace) {
            continue;
        }
        audio::GraphPluginBinding* binding = nullptr;
        if (!entry.workspace_id.empty()) {
            binding = plugin_instance(entry.workspace_id);
            if (binding == nullptr) {
                binding = clap_instance(entry.workspace_id);
            }
            if (binding == nullptr) {
                binding = vst3_instance(entry.workspace_id);
            }
        }
        if (binding == nullptr && !entry.plugin_id.empty()) {
            for (const auto& [workspace_id, candidate] : instances_by_node_) {
                if (candidate->manifest().id == entry.plugin_id) {
                    binding = candidate;
                    break;
                }
            }
        }
        bundle.plugin_by_slot[i + 1] = binding;
    }
}

graph::ConnectResult ProjectSession::add_cable(const graph::CableEnd& source,
                                               const graph::CableEnd& dest, graph::CableMode mode) {
    const std::string cable_id = workspace_.next_cable_id();
    const graph::ConnectResult result = workspace_.connect(source, dest, mode, cable_id);
    if (result != graph::ConnectResult::kOk) {
        return result;
    }
    publish_graph();
    ProjectSession* self = this;
    undo_.push(
        "connect cable",
        [self, cable_id] {
            self->workspace_.disconnect(cable_id);
            self->publish_graph();
        },
        [self, source, dest, mode, cable_id] {
            self->workspace_.connect(source, dest, mode, cable_id);
            self->publish_graph();
        });
    return result;
}

bool ProjectSession::remove_cable(const std::string& cable_id) {
    const graph::Cable* cable = workspace_.find_cable(cable_id);
    if (cable == nullptr) {
        return false;
    }
    const graph::Cable before = *cable;
    workspace_.disconnect(cable_id);
    publish_graph();
    ProjectSession* self = this;
    undo_.push(
        "remove cable",
        [self, before] {
            self->workspace_.connect(before.source, before.dest, before.mode, before.id);
            self->publish_graph();
        },
        [self, before] {
            self->workspace_.disconnect(before.id);
            self->publish_graph();
        });
    return true;
}

bool ProjectSession::set_cable_mode(const std::string& cable_id, graph::CableMode mode) {
    const graph::Cable* cable = workspace_.find_cable(cable_id);
    if (cable == nullptr || cable->mode == mode) {
        return false;
    }
    const graph::CableMode before = cable->mode;
    workspace_.set_cable_mode(cable_id, mode);
    publish_graph();
    ProjectSession* self = this;
    undo_.push(
        "cable mode",
        [self, cable_id, before] {
            self->workspace_.set_cable_mode(cable_id, before);
            self->publish_graph();
        },
        [self, cable_id, mode] {
            self->workspace_.set_cable_mode(cable_id, mode);
            self->publish_graph();
        });
    return true;
}

std::string ProjectSession::add_sum_node() {
    const std::string workspace_id = workspace_.next_node_id("sum");
    workspace_.add_node(graph::make_utility_sum_node(workspace_id));
    publish_graph();
    ProjectSession* self = this;
    undo_.push(
        "add sum node",
        [self, workspace_id] {
            self->workspace_.remove_node(workspace_id);
            self->publish_graph();
        },
        [self, workspace_id] {
            self->workspace_.add_node(graph::make_utility_sum_node(workspace_id));
            self->publish_graph();
        });
    return workspace_id;
}

bool ProjectSession::remove_workspace_node(const std::string& workspace_id) {
    const graph::Node* node = workspace_.find_node(workspace_id);
    if (node == nullptr ||
        (node->kind != graph::NodeKind::kUtilitySum && node->kind != graph::NodeKind::kPlugin)) {
        return false; // built-in nodes are pinned
    }
    instances_by_node_.erase(workspace_id); // instance retires in the registry
    clap_by_node_.erase(workspace_id);      // instance retires in clap_instances_
    clap_editors_.erase(workspace_id);
    vst3_by_node_.erase(workspace_id);
    // Removal rips touching cables; history would need them restored in
    // order, so this clears (consistent with other structural edits).
    workspace_.remove_node(workspace_id);
    undo_.clear();
    publish_graph();
    return true;
}

void ProjectSession::play() {
    audio_.send({.type = audio::Command::Type::kTransportPlay,
                 .value = 0.0F,
                 .sample = nullptr,
                 .bundle = nullptr});
}

void ProjectSession::stop() {
    audio_.send({.type = audio::Command::Type::kTransportStop,
                 .value = 0.0F,
                 .sample = nullptr,
                 .bundle = nullptr});
}

bool ProjectSession::playing() const {
    return audio_.snapshot().transport_playing;
}

void ProjectSession::set_cell(int pattern_index, int row, int channel,
                              const engine::TrackerCell& cell) {
    if (pattern_index < 0 || pattern_index >= static_cast<int>(project_.patterns.size())) {
        return;
    }
    auto& pattern = project_.patterns[static_cast<std::size_t>(pattern_index)];
    if (row < 0 || row >= static_cast<int>(pattern.rows.size())) {
        return;
    }
    auto& cells = pattern.rows[static_cast<std::size_t>(row)];
    if (channel < 0 || channel >= static_cast<int>(cells.size())) {
        return;
    }

    const engine::TrackerCell before = cells[static_cast<std::size_t>(channel)];
    cells[static_cast<std::size_t>(channel)] = cell;

    engine::TrackerProject* project = &project_;
    undo_.push(
        "cell edit",
        [project, pattern_index, row, channel, before] {
            project->patterns[static_cast<std::size_t>(pattern_index)]
                .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)] = before;
        },
        [project, pattern_index, row, channel, cell] {
            project->patterns[static_cast<std::size_t>(pattern_index)]
                .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)] = cell;
        });
}

void ProjectSession::preview_note(int channel, int slot, int note) {
    int sample_id = slot;
    if (!project_.instrument_table.empty() && slot >= 1 &&
        slot <= static_cast<int>(project_.instrument_table.size())) {
        sample_id = project_.instrument_table[static_cast<std::size_t>(slot - 1)].sample_id;
    }
    const audio::SampleBuffer* buffer = sample_buffer(sample_id);
    if (buffer == nullptr) {
        return;
    }
    int base_note = 60;
    for (const engine::TrackerSample& s : project_.samples) {
        if (s.id == sample_id) {
            base_note = s.base_note;
            break;
        }
    }
    const int period = engine::note_to_period(note);
    audio_.send({.type = audio::Command::Type::kPreviewNote,
                 .value = static_cast<float>(engine::period_to_playback_rate(period, base_note)),
                 .sample = buffer,
                 .bundle = nullptr,
                 .aux_int = channel});
}

bool ProjectSession::load_sample_into_slot(int slot, const std::filesystem::path& path) {
    if (slot < 1 || slot > engine::kMaxSamples) {
        error_ = "invalid sample slot";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error_ = "cannot open " + path.string();
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    std::string format;
    std::string decode_error;
    auto buffer = audio::load_sample_memory(
        bytes.data(), bytes.size(), audio_.sample_rate() != 0 ? audio_.sample_rate() : 48000,
        format, decode_error);
    if (buffer == nullptr) {
        error_ = decode_error;
        return false;
    }

    stop();
    engine::TrackerSample meta;
    meta.id = slot;
    meta.name = path.stem().string();
    for (char& c : meta.name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    meta.name.resize(std::min<std::size_t>(meta.name.size(), 22));
    meta.file_name = path.filename().string();
    meta.format = format;
    meta.original_data = bytes;
    meta.sample_rate = buffer->source_rate;
    meta.num_channels = 1;
    meta.frames = buffer->frames;
    buffer->name = meta.name;

    // Replace or insert the slot's metadata (single move at the end
    // keeps the lifetime obvious).
    engine::TrackerSample* dest = nullptr;
    for (engine::TrackerSample& existing : project_.samples) {
        if (existing.id == slot) {
            dest = &existing;
            break;
        }
    }
    if (dest != nullptr) {
        *dest = std::move(meta);
    } else {
        project_.samples.push_back(std::move(meta));
    }
    buffers_[static_cast<std::size_t>(slot)] = std::move(buffer);
    // Structural change: history refers to freed audio, so it clears
    // (matches the web app's slot-load behaviour).
    undo_.clear();
    publish_bundle();
    return true;
}

void ProjectSession::clear_slot(int slot) {
    stop();
    project_.samples.erase(
        std::remove_if(project_.samples.begin(), project_.samples.end(),
                       [slot](const engine::TrackerSample& s) { return s.id == slot; }),
        project_.samples.end());
    if (slot >= 1 && slot <= engine::kMaxSamples) {
        buffers_[static_cast<std::size_t>(slot)].reset();
    }
    undo_.clear();
    publish_bundle();
}

void ProjectSession::set_sample_meta(int slot, const engine::TrackerSample& meta) {
    for (std::size_t i = 0; i < project_.samples.size(); ++i) {
        if (project_.samples[i].id != slot) {
            continue;
        }
        const engine::TrackerSample before = project_.samples[i];
        engine::TrackerSample after = meta;
        // Audio payload fields are owned by load_sample_into_slot.
        after.id = slot;
        after.original_data = before.original_data;
        after.format = before.format;
        after.frames = before.frames;
        after.sample_rate = before.sample_rate;
        project_.samples[i] = after;

        engine::TrackerProject* project = &project_;
        undo_.push(
            "sample properties", [project, i, before] { project->samples[i] = before; },
            [project, i, after] { project->samples[i] = after; });
        return;
    }
}

void ProjectSession::set_instrument_entry(int slot, const engine::InstrumentTableEntry& entry) {
    if (slot < 1 || slot > engine::kMaxSamples) {
        return;
    }
    auto& table = project_.instrument_table;
    const auto old_table = table;
    if (static_cast<int>(table.size()) < slot) {
        table.resize(static_cast<std::size_t>(slot));
        // Fresh entries default to mapping their own slot number.
        for (std::size_t i = old_table.size(); i < table.size(); ++i) {
            table[i].sample_id = static_cast<int>(i) + 1;
        }
    }
    table[static_cast<std::size_t>(slot - 1)] = entry;

    engine::TrackerProject* project = &project_;
    const auto new_table = table;
    undo_.push(
        "instrument entry", [project, old_table] { project->instrument_table = old_table; },
        [project, new_table] { project->instrument_table = new_table; });
    // Plugin slot bindings live in the bundle; refresh them live.
    publish_graph();
}

namespace {

engine::FxChannelStrip make_default_strip(int index, int tracker_channels) {
    engine::FxChannelStrip strip;
    strip.name = "FX " + std::string(index + 1 < 10 ? "0" : "") + std::to_string(index + 1);
    strip.enabled = true;
    strip.tracker_sends.assign(static_cast<std::size_t>(tracker_channels), 1.0F);
    return strip;
}

} // namespace

void ProjectSession::add_fx_channel() {
    stop();
    project_.fx_mixer.channels.push_back(
        make_default_strip(static_cast<int>(project_.fx_mixer.channels.size()), project_.channels));
    undo_.clear();
    publish_bundle();
}

void ProjectSession::remove_fx_channel(int channel) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    stop();
    channels.erase(channels.begin() + channel);
    undo_.clear();
    publish_bundle();
}

void ProjectSession::add_fx_module(int channel, const std::string& module_id) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    const audio::FxModuleDef* def = audio::fx_module_by_id(module_id);
    if (def == nullptr || channels[static_cast<std::size_t>(channel)].modules.size() >= 8) {
        return;
    }
    stop();
    engine::FxModuleInstance instance;
    instance.module_id = module_id;
    for (const audio::FxParamDef& param : def->params) {
        instance.params.emplace_back(param.key, param.def);
    }
    channels[static_cast<std::size_t>(channel)].modules.push_back(std::move(instance));
    undo_.clear();
    publish_bundle();
}

void ProjectSession::remove_fx_module(int channel, int module_index) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    auto& modules = channels[static_cast<std::size_t>(channel)].modules;
    if (module_index < 0 || module_index >= static_cast<int>(modules.size())) {
        return;
    }
    stop();
    modules.erase(modules.begin() + module_index);
    undo_.clear();
    publish_bundle();
}

void ProjectSession::set_fx_module_param(int channel, int module_index, int param_index,
                                         float value) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    auto& modules = channels[static_cast<std::size_t>(channel)].modules;
    if (module_index < 0 || module_index >= static_cast<int>(modules.size())) {
        return;
    }
    auto& params = modules[static_cast<std::size_t>(module_index)].params;
    if (param_index < 0 || param_index >= static_cast<int>(params.size())) {
        return;
    }
    params[static_cast<std::size_t>(param_index)].second = value;
    // Live update to the running rack.
    audio_.send({.type = audio::Command::Type::kFxParam,
                 .value = value,
                 .aux_int = (channel << 16) | (module_index << 8) | param_index});
}

void ProjectSession::set_fx_strip(int channel, int volume, int pan, bool enabled) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    auto& strip = channels[static_cast<std::size_t>(channel)];
    strip.volume = volume;
    strip.pan = pan;
    strip.enabled = enabled;
    stop();
    publish_bundle();
}

void ProjectSession::set_fx_send(int channel, int tracker_channel, float amount) {
    auto& channels = project_.fx_mixer.channels;
    if (channel < 0 || channel >= static_cast<int>(channels.size())) {
        return;
    }
    auto& sends = channels[static_cast<std::size_t>(channel)].tracker_sends;
    if (static_cast<int>(sends.size()) < project_.channels) {
        sends.resize(static_cast<std::size_t>(project_.channels), 0.0F);
    }
    if (tracker_channel < 0 || tracker_channel >= static_cast<int>(sends.size())) {
        return;
    }
    sends[static_cast<std::size_t>(tracker_channel)] = amount;
    stop();
    publish_bundle();
}

const audio::SampleBuffer* ProjectSession::sample_buffer(int sample_id) const {
    if (sample_id < 1 || sample_id > engine::kMaxSamples) {
        return nullptr;
    }
    return buffers_[static_cast<std::size_t>(sample_id)].get();
}

} // namespace nt::app
