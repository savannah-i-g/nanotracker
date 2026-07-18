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
            if (ext::ClapPlugin* clap = session.clap_instance(node.workspace_id)) {
                // External plugins get the auto-param panel (editor OS
                // windows attach through the gui extension separately).
                for (const ext::ClapParamInfo& param : clap->params()) {
                    auto value = static_cast<float>(clap->param_value(param.id));
                    // Display names may repeat ("Rate" et al); the
                    // stable param id keys the widget.
                    ImGui::PushID(static_cast<int>(param.id));
                    ImGui::SetNextItemWidth(160.0F);
                    if (ImGui::SliderFloat(param.name.c_str(), &value,
                                           static_cast<float>(param.min),
                                           static_cast<float>(param.max), "%.3f")) {
                        clap->set_param(param.id, value);
                    }
                    ImGui::PopID();
                }
                if (clap->params().empty()) {
                    ImGui::TextDisabled("no parameters");
                }
                if (session.clap_editor_open(node.workspace_id)) {
                    ImGui::TextDisabled("editor open");
                } else if (ImGui::SmallButton("open editor")) {
                    if (!session.open_clap_editor(node.workspace_id)) {
                        status_ = session.error();
                        status_ttl_ = 6.0F;
                    }
                }
            } else if (ext::Vst3Plugin* vst3 = session.vst3_instance(node.workspace_id)) {
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
                if (session.vst3_editor_open(node.workspace_id)) {
                    ImGui::TextDisabled("editor open");
                } else if (ImGui::SmallButton("open editor")) {
                    if (!session.open_vst3_editor(node.workspace_id)) {
                        status_ = session.error();
                        status_ttl_ = 6.0F;
                    }
                }
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
                session.add_clap_node(entry.descriptor.id);
            }
            ImGui::PopID();
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
