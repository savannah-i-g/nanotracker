#include "ui/workspace_view.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace nt::ui {

namespace {

constexpr float kJackRadius = 5.0F;
constexpr float kJackHitSize = 16.0F;

// Jack fill colours by kind — matches the cable palette so a jack
// telegraphs what plugs into it.
ImU32 jack_colour(graph::PortKind kind) {
    switch (kind) {
    case graph::PortKind::kAudio:
        return IM_COL32(0xFF, 0x66, 0x00, 0xFF);
    case graph::PortKind::kSidechain:
        return IM_COL32(0xFF, 0x99, 0x55, 0xFF);
    case graph::PortKind::kCv:
        return IM_COL32(0xA0, 0xE0, 0x60, 0xFF);
    case graph::PortKind::kGate:
        return IM_COL32(0x60, 0xC0, 0xFF, 0xFF);
    case graph::PortKind::kMidi:
        return IM_COL32(0xE6, 0xB8, 0x4A, 0xFF);
    }
    return IM_COL32_WHITE;
}

} // namespace

void WorkspaceView::draw_jack(const graph::Node& node, const graph::Port& port, bool is_input,
                              const Theme& theme) {
    ImGui::PushID(port.id.c_str());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 center{cursor.x + (kJackHitSize * 0.5F), cursor.y + (kJackHitSize * 0.5F)};
    ImGui::InvisibleButton("jack", ImVec2{kJackHitSize, kJackHitSize});
    const bool hovered = ImGui::IsItemHovered();

    // Drop-target validity feedback while a drag is live (fix #13):
    // compatible inputs glow green, incompatible ones red.
    ImU32 ring = IM_COL32(0, 0, 0, 0);
    if (overlay_.dragging() && is_input) {
        const bool ok = graph::port_kinds_compatible(overlay_.drag_source().kind, port.kind);
        ring = ok ? IM_COL32(0x60, 0xFF, 0x60, 0xFF) : IM_COL32(0xFF, 0x40, 0x40, 0xFF);
    } else if (hovered) {
        ring = ImGui::GetColorU32(theme.primary);
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(center, kJackRadius, jack_colour(port.kind));
    draw->AddCircle(center, kJackRadius + 2.0F, ring, 0, 2.0F);

    if (hovered) {
        ImGui::SetTooltip("%s (%s %s)", port.label.c_str(), graph::port_kind_name(port.kind),
                          is_input ? "in" : "out");
    }
    // Dragging out of an output jack starts a cable.
    if (!is_input && !overlay_.dragging() && ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0F)) {
        overlay_.begin_drag({.node_id = node.workspace_id,
                             .port_id = port.id,
                             .kind = port.kind,
                             .is_input = false,
                             .pos = center});
    }

    anchors_.push_back({.node_id = node.workspace_id,
                        .port_id = port.id,
                        .kind = port.kind,
                        .is_input = is_input,
                        .pos = center});
    ImGui::PopID();
}

void WorkspaceView::draw_collapsed_jack_strip(const graph::Node& node) {
    // Minimised windows keep cables anchored: a compact dot strip along
    // the titlebar, outputs from the right edge inward, inputs from the
    // left. Foreground list — the titlebar itself is already drawn.
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float y = pos.y + (ImGui::GetFrameHeight() * 0.5F);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    constexpr float kStep = 9.0F;
    float x = pos.x + size.x - 26.0F;
    for (const graph::Port& port : node.outputs) {
        draw->AddCircleFilled(ImVec2{x, y}, 3.0F, jack_colour(port.kind));
        anchors_.push_back({.node_id = node.workspace_id,
                            .port_id = port.id,
                            .kind = port.kind,
                            .is_input = false,
                            .pos = ImVec2{x, y}});
        x -= kStep;
    }
    float xi = pos.x + 26.0F;
    for (const graph::Port& port : node.inputs) {
        draw->AddCircleFilled(ImVec2{xi, y}, 3.0F, jack_colour(port.kind));
        anchors_.push_back({.node_id = node.workspace_id,
                            .port_id = port.id,
                            .kind = port.kind,
                            .is_input = true,
                            .pos = ImVec2{xi, y}});
        xi += kStep;
    }
}

bool WorkspaceView::draw_node_window(app::ProjectSession& session, graph::Node& node,
                                     const Theme& theme, int layout_generation) {
    const std::string title = node.display_name + "###ws_" + node.workspace_id;

    // Loaded placements apply once per layout generation; afterwards
    // ImGui owns the geometry and we read it back into the model.
    const ImGuiCond cond =
        layout_generation != seen_layout_generation_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2{node.window.x, node.window.y}, cond);
    ImGui::SetNextWindowCollapsed(node.window.minimised, cond);

    const bool closable =
        node.kind == graph::NodeKind::kUtilitySum || node.kind == graph::NodeKind::kPlugin;
    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize;
    const bool expanded = ImGui::Begin(title.c_str(), closable ? &open : nullptr, flags);

    // Geometry writes back every frame so project saves capture layout.
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    node.window.x = pos.x;
    node.window.y = pos.y;
    node.window.width = size.x;
    node.window.height = size.y;
    node.window.minimised = ImGui::IsWindowCollapsed();

    if (expanded) {
        // Jack rails: inputs on the left column, outputs on the right.
        ImGui::BeginGroup();
        if (node.inputs.empty()) {
            ImGui::TextDisabled(" ");
        }
        for (const graph::Port& port : node.inputs) {
            draw_jack(node, port, true, theme);
            ImGui::SameLine();
            ImGui::TextUnformatted(port.label.c_str());
        }
        ImGui::EndGroup();
        ImGui::SameLine(0.0F, 24.0F);
        ImGui::BeginGroup();
        for (const graph::Port& port : node.outputs) {
            // Label left of the jack so the jack sits on the right rail.
            ImGui::TextUnformatted(port.label.c_str());
            ImGui::SameLine();
            draw_jack(node, port, false, theme);
        }
        ImGui::EndGroup();

        // Plugin body: declarative controls (or the auto-param panel)
        // under the jack rails, plus the master-route strip. Strip
        // edits republish once the drag ends (the runner snapshots
        // volume/pan/bypass at build).
        if (node.kind == graph::NodeKind::kPlugin) {
            ImGui::Separator();
            if (session.is_external_plugin_node(node.workspace_id)) {
                // External CLAP/VST3: the bridge opt-in + (bridged) crash
                // badge, else the in-process auto-param panel.
                draw_external_plugin_body(session, node, theme);
            } else {
                plugin_ui_.draw(session, node.workspace_id, theme);
            }
            ImGui::Separator();
            bool strip_changed = false;
            ImGui::SetNextItemWidth(90.0F);
            ImGui::SliderFloat("vol", &node.volume, 0.0F, 1.0F, "%.2f");
            strip_changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0F);
            ImGui::SliderFloat("pan", &node.pan, -1.0F, 1.0F, "%.2f");
            strip_changed |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SameLine();
            strip_changed |= ImGui::Checkbox("byp", &node.bypass);
            if (strip_changed) {
                session.publish_workspace_strips();
            }
        }
    } else {
        draw_collapsed_jack_strip(node);
    }
    ImGui::End();

    return closable && !open;
}

void WorkspaceView::draw_external_plugin_body(app::ProjectSession& session, graph::Node& node,
                                              const Theme& theme) {
    const std::string& ws = node.workspace_id;
    const bool bridged = session.external_plugin_bridged(ws);

    if (bridged) {
        // The badge reads the bridge's live_state off the audio thread (the
        // reaper drives the transition each frame in ProjectSession::
        // update_bridged). Live is subtle; not-responding is amber; a
        // reaper-confirmed crash is loud red with a one-click restart.
        ext::bridge::BridgedPlugin* binding = session.bridged_plugin(ws);
        const ext::bridge::LiveState state =
            binding != nullptr ? binding->live_state() : ext::bridge::LiveState::kLive;
        if (state == ext::bridge::LiveState::kLive) {
            ImGui::TextColored(theme.primary, "[bridged]");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "hosted out-of-process — a crash here can't take down the tracker");
            }
        } else if (state == ext::bridge::LiveState::kBypassed) {
            ImGui::TextColored(ImVec4{0.95F, 0.65F, 0.15F, 1.0F}, "plugin not responding");
        } else { // kCrashed
            ImGui::TextColored(ImVec4{0.96F, 0.26F, 0.26F, 1.0F}, "plugin crashed —");
            ImGui::SameLine();
            if (ImGui::SmallButton("Restart")) {
                if (!session.restart_bridged_plugin(ws)) {
                    status_ = session.error();
                    status_ttl_ = 6.0F;
                }
            }
        }
        // Editor: the bridge's cross-process reparented container (S29d),
        // not the in-process ClapEditorWindow.
        if (session.bridged_editor_open(ws)) {
            ImGui::TextDisabled("editor open");
        } else if (ImGui::SmallButton("open editor")) {
            if (!session.open_bridged_editor(ws)) {
                status_ = session.error();
                status_ttl_ = 6.0F;
            }
        }
    } else if (ext::ClapPlugin* clap = session.clap_instance(ws)) {
        // In-process CLAP: the auto-param panel (editor OS windows attach
        // through the gui extension separately).
        for (const ext::ClapParamInfo& param : clap->params()) {
            auto value = static_cast<float>(clap->param_value(param.id));
            // Display names may repeat ("Rate" et al); the stable param id
            // keys the widget.
            ImGui::PushID(static_cast<int>(param.id));
            ImGui::SetNextItemWidth(160.0F);
            if (ImGui::SliderFloat(param.name.c_str(), &value, static_cast<float>(param.min),
                                   static_cast<float>(param.max), "%.3f")) {
                clap->set_param(param.id, value);
            }
            ImGui::PopID();
        }
        if (clap->params().empty()) {
            ImGui::TextDisabled("no parameters");
        }
        if (session.clap_editor_open(ws)) {
            ImGui::TextDisabled("editor open");
        } else if (ImGui::SmallButton("open editor")) {
            if (!session.open_clap_editor(ws)) {
                status_ = session.error();
                status_ttl_ = 6.0F;
            }
        }
    } else if (ext::Vst3Plugin* vst3 = session.vst3_instance(ws)) {
        int shown = 0;
        for (const ext::Vst3ParamInfo& param : vst3->params()) {
            if (!param.automatable || shown >= 24) {
                continue; // synth params number in the hundreds
            }
            auto value = static_cast<float>(vst3->param_value(param.id));
            // Same duplicate-title hazard as the CLAP panel.
            ImGui::PushID(static_cast<int>(param.id));
            ImGui::SetNextItemWidth(160.0F);
            if (ImGui::SliderFloat(param.title.c_str(), &value, 0.0F, 1.0F, "%.3f")) {
                vst3->set_param(param.id, value);
            }
            ImGui::PopID();
            ++shown;
        }
        if (static_cast<int>(vst3->params().size()) > shown) {
            ImGui::TextDisabled("showing %d of %d parameters", shown,
                                static_cast<int>(vst3->params().size()));
        }
        if (session.vst3_editor_open(ws)) {
            ImGui::TextDisabled("editor open");
        } else if (ImGui::SmallButton("open editor")) {
            if (!session.open_vst3_editor(ws)) {
                status_ = session.error();
                status_ttl_ = 6.0F;
            }
        }
    }

    // Crash-isolation opt-in. Enabled on CLAP nodes; shown disabled on VST3
    // (VST3-in-child is a planned follow-up). Flipping re-creates the
    // instance bridged/in-process, carrying its state across.
    const bool bridgeable = session.external_plugin_bridgeable(ws);
    bool want = bridged;
    if (!bridgeable) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("run bridged (crash-isolated)", &want)) {
        if (!session.set_external_plugin_bridged(ws, want)) {
            status_ = session.error();
            status_ttl_ = 6.0F;
        }
    }
    if (!bridgeable) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(bridgeable ? "host this plugin in a separate process so a crash can't\n"
                                       "take down the tracker. Adds ~2.67 ms (one block) of\n"
                                       "latency; the plugin editor embeds on Linux only."
                                     : "out-of-process hosting is CLAP-only in this release;\n"
                                       "VST3-in-child is a planned follow-up");
    }
}

bool WorkspaceView::draw_control_window(app::ProjectSession& session, io::Settings& settings,
                                        const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2{300, 0}, ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("WORKSPACE");
    if (visible) {
        if (ImGui::Button("add SUM node")) {
            session.add_sum_node();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d cables", static_cast<int>(session.workspace().cables().size()));

        // Plugin loading + instantiation.
        static std::array<char, 512> plugin_path{};
        ImGui::SetNextItemWidth(190.0F);
        ImGui::InputTextWithHint("##ntp", "path/to/plugin.ntins", plugin_path.data(),
                                 plugin_path.size());
        ImGui::SameLine();
        if (ImGui::Button("load plugin")) {
            if (session.load_plugin_file(plugin_path.data()).empty()) {
                status_ = session.error();
                status_ttl_ = 6.0F;
            }
        }
        for (const auto& plugin : session.plugin_registry().all()) {
            ImGui::PushID(plugin->manifest.id.c_str());
            ImGui::TextUnformatted(plugin->manifest.name.c_str());
            // Trust surface (Plan_PostV1/10): native-stage archives
            // execute code — marked in the catalogue, never silent.
            if (plugin->executes_native_code) {
                ImGui::SameLine();
                ImGui::TextColored(theme.primary, "[native code]");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("this plugin executes native code shipped in its archive\n"
                                      "load it only if you trust the author");
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("add to workspace")) {
                session.add_plugin_node(plugin->manifest.id);
            }
            ImGui::PopID();
        }

        // External (CLAP) plugins share the same flow: load a library,
        // instantiate as a workspace node.
        static std::array<char, 512> clap_path{};
        ImGui::SetNextItemWidth(190.0F);
        ImGui::InputTextWithHint("##clap", "path/to/plugin.clap", clap_path.data(),
                                 clap_path.size());
        ImGui::SameLine();
        if (ImGui::Button("load CLAP")) {
            if (!session.load_clap_file(clap_path.data())) {
                status_ = session.error();
                status_ttl_ = 6.0F;
            }
        }
        for (const auto& entry : session.vst3_catalog()) {
            ImGui::PushID(entry.descriptor.id.c_str());
            ImGui::TextUnformatted(entry.descriptor.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(VST3)");
            ImGui::SameLine();
            if (ImGui::SmallButton("add to workspace")) {
                session.add_vst3_node(entry.descriptor.id);
            }
            ImGui::PopID();
        }
        for (const auto& entry : session.clap_catalog()) {
            ImGui::PushID(entry.descriptor.id.c_str());
            ImGui::TextUnformatted(entry.descriptor.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(CLAP)");
            ImGui::SameLine();
            if (ImGui::SmallButton("add to workspace")) {
                // New CLAP nodes seed from the global default; each node's
                // choice is then editable on the node and saved in the project.
                session.add_clap_node(entry.descriptor.id, settings.bridge_external_by_default);
            }
            ImGui::PopID();
        }

        // Global default for the per-node crash-isolation opt-in (§E/§H:
        // off by default). The per-instance choice persists in the project.
        ImGui::Checkbox("bridge external plugins by default", &settings.bridge_external_by_default);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("new CLAP/VST3 nodes start crash-isolated (out-of-process).\n"
                              "Off by default — bridging adds one block of latency.");
        }

        if (status_ttl_ > 0.0F && !status_.empty()) {
            ImGui::TextWrapped("%s", status_.c_str());
        }

        if (ImGui::CollapsingHeader("cable physics")) {
            ImGui::SliderInt("resolution", &settings.cable_resolution, 1, 64);
            ImGui::SliderFloat("gravity", &settings.cable_gravity, 0.0F, 2400.0F, "%.0f");
            ImGui::SliderFloat("damping", &settings.cable_damping, 0.0F, 0.95F, "%.2f");
            ImGui::SliderFloat("slack", &settings.cable_slack, 0.5F, 3.0F, "%.2f");
            ImGui::SliderInt("iterations", &settings.cable_iterations, 1, 16);
            ImGui::SliderFloat("thickness", &settings.cable_thickness, 0.5F, 16.0F, "%.1f");
        }
    }
    ImGui::End();
    return visible;
}

void WorkspaceView::draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme) {
    anchors_.clear();

    // The patchbay infrastructure (bus/master/module/sum/ext nodes +
    // the cable overlay) shows only while the workspace window itself
    // is visible — in the docked layout those panels would otherwise
    // float over whichever center tab is active. Plugin instrument
    // windows are working surfaces, not patchbay furniture: they stay
    // up regardless of the active tab.
    const bool patchbay_visible = draw_control_window(session, settings, theme);

    // Node windows write geometry back into the model; close requests
    // collect during the loop and dispatch after so the vector stays
    // stable while iterated.
    std::vector<graph::Node>& nodes = session.workspace().nodes_mut();
    const int generation = session.workspace_layout_generation();
    std::string close_requested;
    for (graph::Node& node : nodes) {
        if (!patchbay_visible && node.kind != graph::NodeKind::kPlugin) {
            continue;
        }
        if (draw_node_window(session, node, theme, generation)) {
            close_requested = node.workspace_id;
        }
    }
    seen_layout_generation_ = generation;
    if (!close_requested.empty()) {
        session.remove_workspace_node(close_requested);
    }

    if (patchbay_visible) {
        std::string message;
        overlay_.draw(session, anchors_, settings, theme.light, message);
        if (!message.empty()) {
            status_ = message;
            status_ttl_ = 4.0F;
        }
    }
    status_ttl_ = std::max(0.0F, status_ttl_ - ImGui::GetIO().DeltaTime);
}

} // namespace nt::ui
