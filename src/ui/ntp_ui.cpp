#include "ui/ntp_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glad/gl.h>
#include <imgui.h>
#include <numbers>
#include <vector>

namespace nt::ui {

namespace {

const ntp::ParamDef* param_def(const ntp::Manifest& manifest, const std::string& key) {
    for (const ntp::ParamDef& def : manifest.params) {
        if (def.key == key) {
            return &def;
        }
    }
    return nullptr;
}

// Rotary knob: vertical drag adjusts, draw-list arc renders. Returns
// true when the value changed.
bool knob(const char* label, float* value, float min, float max, float size) {
    ImGui::PushID(label);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float diameter = size;
    ImGui::InvisibleButton("knob", ImVec2{diameter, diameter + ImGui::GetTextLineHeight()});
    bool changed = false;
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0.0F) {
        const float range = max - min;
        *value = std::clamp(*value - (ImGui::GetIO().MouseDelta.y * range * 0.005F), min, max);
        changed = true;
    }
    const float t = max > min ? (*value - min) / (max - min) : 0.0F;
    const ImVec2 center{origin.x + (diameter * 0.5F), origin.y + (diameter * 0.5F)};
    const float radius = diameter * 0.42F;
    // 270-degree sweep, 7 o'clock to 5 o'clock.
    const float start = 0.75F * std::numbers::pi_v<float>;
    const float sweep = 1.5F * std::numbers::pi_v<float>;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PathArcTo(center, radius, start, start + sweep, 24);
    draw->PathStroke(ImGui::GetColorU32(ImGuiCol_FrameBg), ImDrawFlags_None, 3.0F);
    draw->PathArcTo(center, radius, start, start + (sweep * t), 24);
    draw->PathStroke(ImGui::GetColorU32(ImGuiCol_SliderGrab), ImDrawFlags_None, 3.0F);
    const float angle = start + (sweep * t);
    draw->AddLine(center,
                  ImVec2{center.x + (std::cos(angle) * radius * 0.7F),
                         center.y + (std::sin(angle) * radius * 0.7F)},
                  ImGui::GetColorU32(ImGuiCol_Text), 2.0F);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2{center.x - (text_size.x * 0.5F), origin.y + diameter},
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.3f", static_cast<double>(*value));
    }
    ImGui::PopID();
    return changed;
}

} // namespace

NtpUi::Texture& NtpUi::texture_for(const plugins::LoadedNtpPlugin& plugin,
                                   const std::string& asset) {
    const std::string key = plugin.manifest.id + "/" + asset;
    auto it = textures_.find(key);
    if (it != textures_.end()) {
        return it->second;
    }
    Texture texture;
    const auto image_it = plugin.images.find(asset);
    if (image_it != plugin.images.end()) {
        const plugins::LoadedNtpPlugin::Image& image = image_it->second;
        glGenTextures(1, &texture.id);
        glBindTexture(GL_TEXTURE_2D, texture.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, image.rgba.data());
        texture.width = image.width;
        texture.height = image.height;
    }
    return textures_.emplace(key, texture).first->second;
}

void NtpUi::draw_param_widget(plugins::NtpInstance& instance, const ntp::ParamDef& def,
                              bool as_knob, float width_hint) {
    float value = instance.param_value(def.key);
    bool changed = false;
    if (as_knob) {
        changed = knob(def.label.c_str(), &value, static_cast<float>(def.min),
                       static_cast<float>(def.max), width_hint > 0.0F ? width_hint : 42.0F);
    } else {
        ImGui::SetNextItemWidth(width_hint > 0.0F ? width_hint : 160.0F);
        ImGuiSliderFlags flags = ImGuiSliderFlags_None;
        if (def.curve != ntp::ParamCurve::kLinear && def.min > 0.0) {
            flags |= ImGuiSliderFlags_Logarithmic;
        }
        changed = ImGui::SliderFloat(def.label.c_str(), &value, static_cast<float>(def.min),
                                     static_cast<float>(def.max), "%.3f", flags);
    }
    if (changed) {
        if (def.step > 0.0) {
            value =
                static_cast<float>(def.min) +
                (std::round((value - static_cast<float>(def.min)) / static_cast<float>(def.step)) *
                 static_cast<float>(def.step));
        }
        instance.set_param(def.key, value);
    }
}

void NtpUi::queue_animation(const std::string& workspace_id, const ntp::UiControl& control) {
    // Activation edge = one trigger per interaction gesture (a drag
    // retriggering every frame would pin the one-shot on its first
    // frame). The owning sprite consumes the key when it draws.
    if (!control.animation.empty() && ImGui::IsItemActivated()) {
        pending_triggers_.insert(workspace_id + "/" + control.animation);
    }
}

// Control trees are recursive by design (loader bounds depth to 4).
// NOLINTNEXTLINE(misc-no-recursion)
void NtpUi::draw_control(app::ProjectSession& session, const std::string& workspace_id,
                         plugins::NtpInstance& instance, const ntp::Manifest& manifest,
                         const plugins::LoadedNtpPlugin& plugin, const ntp::UiControl& control,
                         const Theme& theme) {
    switch (control.type) {
    case ntp::ControlType::kKnob:
    case ntp::ControlType::kSlider:
    case ntp::ControlType::kNumber: {
        const ntp::ParamDef* def = param_def(manifest, control.parameter);
        if (def == nullptr) {
            break;
        }
        if (control.type == ntp::ControlType::kNumber) {
            float value = instance.param_value(def->key);
            ImGui::SetNextItemWidth(control.width > 0.0F ? control.width : 90.0F);
            if (ImGui::DragFloat(
                    def->label.c_str(), &value,
                    static_cast<float>(def->step) > 0.0F ? static_cast<float>(def->step) : 0.01F,
                    static_cast<float>(def->min), static_cast<float>(def->max))) {
                instance.set_param(def->key, value);
            }
        } else {
            draw_param_widget(instance, *def, control.type == ntp::ControlType::kKnob,
                              control.width);
        }
        queue_animation(workspace_id, control);
        break;
    }
    case ntp::ControlType::kToggle: {
        const ntp::ParamDef* def = param_def(manifest, control.parameter);
        if (def == nullptr) {
            break;
        }
        bool on = instance.param_value(def->key) >= 0.5F;
        if (ImGui::Checkbox(def->label.c_str(), &on)) {
            instance.set_param(def->key,
                               on ? static_cast<float>(def->max) : static_cast<float>(def->min));
        }
        queue_animation(workspace_id, control);
        break;
    }
    case ntp::ControlType::kSelect: {
        const ntp::ParamDef* def = param_def(manifest, control.parameter);
        if (def == nullptr || control.options.empty()) {
            break;
        }
        const int index = std::clamp(static_cast<int>(instance.param_value(def->key)), 0,
                                     static_cast<int>(control.options.size()) - 1);
        ImGui::SetNextItemWidth(control.width > 0.0F ? control.width : 130.0F);
        if (ImGui::BeginCombo(def->label.c_str(),
                              control.options[static_cast<std::size_t>(index)].c_str())) {
            for (int i = 0; i < static_cast<int>(control.options.size()); ++i) {
                if (ImGui::Selectable(control.options[static_cast<std::size_t>(i)].c_str(),
                                      i == index)) {
                    instance.set_param(def->key, static_cast<float>(i));
                    // Selects trigger on choice, not on opening the popup.
                    if (!control.animation.empty()) {
                        pending_triggers_.insert(workspace_id + "/" + control.animation);
                    }
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case ntp::ControlType::kXyPad: {
        const ntp::ParamDef* def_x = param_def(manifest, control.parameter_x);
        const ntp::ParamDef* def_y = param_def(manifest, control.parameter_y);
        if (def_x == nullptr || def_y == nullptr) {
            break;
        }
        const float width = control.width > 0.0F ? control.width : 120.0F;
        const float height = control.height > 0.0F ? control.height : 120.0F;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("xy", ImVec2{width, height});
        const float vx = instance.param_value(def_x->key);
        const float vy = instance.param_value(def_y->key);
        if (ImGui::IsItemActive()) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const float nx = std::clamp((mouse.x - origin.x) / width, 0.0F, 1.0F);
            const float ny = 1.0F - std::clamp((mouse.y - origin.y) / height, 0.0F, 1.0F);
            instance.set_param(def_x->key, static_cast<float>(def_x->min) +
                                               (nx * static_cast<float>(def_x->max - def_x->min)));
            instance.set_param(def_y->key, static_cast<float>(def_y->min) +
                                               (ny * static_cast<float>(def_y->max - def_y->min)));
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect(origin, ImVec2{origin.x + width, origin.y + height},
                      ImGui::GetColorU32(ImGuiCol_Border));
        const float tx = def_x->max > def_x->min ? (vx - static_cast<float>(def_x->min)) /
                                                       static_cast<float>(def_x->max - def_x->min)
                                                 : 0.0F;
        const float ty = def_y->max > def_y->min ? (vy - static_cast<float>(def_y->min)) /
                                                       static_cast<float>(def_y->max - def_y->min)
                                                 : 0.0F;
        draw->AddCircleFilled(ImVec2{origin.x + (tx * width), origin.y + ((1.0F - ty) * height)},
                              4.0F, ImGui::GetColorU32(theme.primary));
        queue_animation(workspace_id, control);
        break;
    }
    case ntp::ControlType::kEnvelopeEditor:
        draw_envelope_editor(session, workspace_id, manifest, control, theme);
        break;
    case ntp::ControlType::kMeter: {
        const float peak = instance.node_peak(control.node);
        ImGui::ProgressBar(std::clamp(peak, 0.0F, 1.0F),
                           ImVec2{control.width > 0.0F ? control.width : 120.0F, 6.0F}, "");
        break;
    }
    case ntp::ControlType::kLabel:
        ImGui::TextDisabled("%s", control.label.c_str());
        break;
    case ntp::ControlType::kGroup: {
        ImGui::BeginGroup();
        for (std::size_t i = 0; i < control.children.size(); ++i) {
            if (control.row_layout && i > 0) {
                ImGui::SameLine();
            }
            draw_control(session, workspace_id, instance, manifest, plugin, control.children[i],
                         theme);
        }
        ImGui::EndGroup();
        break;
    }
    case ntp::ControlType::kImage: {
        const Texture& texture = texture_for(plugin, control.asset);
        if (texture.id != 0) {
            const float width =
                control.width > 0.0F ? control.width : static_cast<float>(texture.width);
            const float height =
                control.height > 0.0F ? control.height : static_cast<float>(texture.height);
            ImGui::Image(static_cast<ImTextureID>(texture.id), ImVec2{width, height});
        }
        break;
    }
    case ntp::ControlType::kSprite:
        draw_sprite(workspace_id, plugin, control);
        break;
    }
}

void NtpUi::draw_sprite(const std::string& workspace_id, const plugins::LoadedNtpPlugin& plugin,
                        const ntp::UiControl& control) {
    const Texture& texture = texture_for(plugin, control.asset);
    if (texture.id == 0) {
        return;
    }
    // No frame grid: the whole image is one static frame, exactly the
    // pre-animation sprite behaviour.
    if (control.frame_width <= 0 || control.frame_height <= 0) {
        const float width =
            control.width > 0.0F ? control.width : static_cast<float>(texture.width);
        const float height =
            control.height > 0.0F ? control.height : static_cast<float>(texture.height);
        ImGui::Image(static_cast<ImTextureID>(texture.id), ImVec2{width, height});
        return;
    }

    // Frame selection (web pluginSprite.ts semantics): a triggered
    // one-shot runs at its fps and wins; otherwise the first loop
    // animation idles; otherwise frame 0 stands still.
    const double now = ImGui::GetTime();
    int frame = 0;
    if (!control.animations.empty()) {
        SpriteState& state = sprite_state_[workspace_id + "/" + control.animations[0].name];
        for (std::size_t a = 0; a < control.animations.size(); ++a) {
            const auto pending =
                pending_triggers_.find(workspace_id + "/" + control.animations[a].name);
            if (pending != pending_triggers_.end()) {
                pending_triggers_.erase(pending);
                state.anim = static_cast<int>(a);
                state.start = now;
            }
        }
        if (state.anim >= 0) {
            const ntp::SpriteAnimation& anim =
                control.animations[static_cast<std::size_t>(state.anim)];
            const auto step = static_cast<std::size_t>((now - state.start) * anim.fps);
            if (step < anim.frames.size()) {
                frame = anim.frames[step];
            } else {
                state.anim = -1; // one-shot finished — back to idle
            }
        }
        if (state.anim < 0) {
            for (const ntp::SpriteAnimation& anim : control.animations) {
                if (anim.loop) {
                    frame =
                        anim.frames[static_cast<std::size_t>(now * anim.fps) % anim.frames.size()];
                    break;
                }
            }
        }
    }

    // Row-major cell lookup in the sheet (frame indices are validated
    // against the grid capacity at load).
    const int cols = std::max(1, texture.width / control.frame_width);
    const int row = frame / cols; // integer grid coordinates by design
    const auto cell_x = static_cast<float>((frame % cols) * control.frame_width);
    const auto cell_y = static_cast<float>(row * control.frame_height);
    const ImVec2 uv0{cell_x / static_cast<float>(texture.width),
                     cell_y / static_cast<float>(texture.height)};
    const ImVec2 uv1{
        (cell_x + static_cast<float>(control.frame_width)) / static_cast<float>(texture.width),
        (cell_y + static_cast<float>(control.frame_height)) / static_cast<float>(texture.height)};
    const float width =
        control.width > 0.0F ? control.width : static_cast<float>(control.frame_width);
    const float height =
        control.height > 0.0F ? control.height : static_cast<float>(control.frame_height);
    ImGui::Image(static_cast<ImTextureID>(texture.id), ImVec2{width, height}, uv0, uv1);
}

void NtpUi::draw_envelope_editor(app::ProjectSession& session, const std::string& workspace_id,
                                 const ntp::Manifest& manifest, const ntp::UiControl& control,
                                 const Theme& theme) {
    // The edited node: `node` when it names an envelope, else the
    // first envelope in the graph. (The web editor moved ADSR *params*
    // by suffix convention; native edits the stage array itself — the
    // multi-stage editor the format actually stores.)
    const ntp::DspNode* env = nullptr;
    for (const ntp::DspNode& node : manifest.graph.nodes) {
        if (node.type != ntp::NodeType::kEnvelope) {
            continue;
        }
        if (env == nullptr) {
            env = &node;
        }
        if (!control.node.empty() && node.id == control.node) {
            env = &node;
            break;
        }
    }
    if (env == nullptr || env->env_stages.empty()) {
        return;
    }
    // The curve to draw and grab is this instance's — the manifest holds
    // the authored defaults; per-instance edits live on the instance
    // (NtpInstance::effective_env_stages).
    std::vector<ntp::EnvelopeStage> live_stages = env->env_stages;
    if (const plugins::NtpInstance* inst = session.plugin_instance(workspace_id)) {
        std::vector<ntp::EnvelopeStage> effective = inst->effective_env_stages(env->id);
        if (!effective.empty()) {
            live_stages = std::move(effective);
        }
    }

    const float width = control.width > 0.0F ? control.width : 160.0F;
    const float height = control.height > 0.0F ? control.height : 56.0F;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(env->id.c_str());
    ImGui::InvisibleButton("env", ImVec2{width, height});

    // The time axis spans the stages plus the release tail; the point
    // for stage i sits at its cumulative time. During a drag the axis
    // freezes at its start-of-drag scale so the point tracks the mouse
    // instead of rubber-banding as its own time changes.
    auto axis_total = [env](const std::vector<ntp::EnvelopeStage>& stages) {
        double total = 0.0;
        for (const ntp::EnvelopeStage& stage : stages) {
            total += std::max(0.001, stage.time);
        }
        return total + std::max(0.001, env->release);
    };
    auto stage_point = [&](const std::vector<ntp::EnvelopeStage>& stages, int index, double total) {
        double at = 0.0;
        for (int i = 0; i <= index; ++i) {
            at += std::max(0.001, stages[static_cast<std::size_t>(i)].time);
        }
        const auto level = static_cast<float>(stages[static_cast<std::size_t>(index)].target);
        return ImVec2{
            std::min(origin.x + (static_cast<float>(at / total) * width), origin.x + width),
            origin.y + ((1.0F - level) * height)};
    };

    const std::string drag_key = workspace_id + "/" + env->id;
    bool dragging = env_drag_.stage >= 0 && env_drag_.key == drag_key;

    // Drag lifecycle before drawing so the curve reflects this frame.
    if (!dragging && ImGui::IsItemActivated()) {
        const double total = axis_total(live_stages);
        const ImVec2 mouse = ImGui::GetMousePos();
        int best = -1;
        float best_d2 = 10.0F * 10.0F; // grab radius, squared
        for (int i = 0; i < static_cast<int>(live_stages.size()); ++i) {
            const ImVec2 point = stage_point(live_stages, i, total);
            const float dx = mouse.x - point.x;
            const float dy = mouse.y - point.y;
            const float d2 = (dx * dx) + (dy * dy);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = i;
            }
        }
        if (best >= 0) {
            env_drag_ = {.key = drag_key, .stage = best, .total = total, .preview = live_stages};
            dragging = true;
        }
    }
    if (dragging && ImGui::IsItemActive()) {
        const ImVec2 mouse = ImGui::GetMousePos();
        ntp::EnvelopeStage& stage = env_drag_.preview[static_cast<std::size_t>(env_drag_.stage)];
        stage.target = 1.0F - std::clamp((mouse.y - origin.y) / height, 0.0F, 1.0F);
        double prior = 0.0;
        for (int i = 0; i < env_drag_.stage; ++i) {
            prior += std::max(0.001, env_drag_.preview[static_cast<std::size_t>(i)].time);
        }
        const double at =
            static_cast<double>(std::clamp((mouse.x - origin.x) / width, 0.0F, 1.0F)) *
            env_drag_.total;
        stage.time = std::max(0.001, at - prior);
    }
    if (dragging && ImGui::IsItemDeactivated()) {
        // The one republish of the drag (structural — the session
        // stops the transport and republishes the bundle).
        const ntp::EnvelopeStage& stage =
            env_drag_.preview[static_cast<std::size_t>(env_drag_.stage)];
        session.set_plugin_env_stage(workspace_id, env->id, env_drag_.stage, stage.target,
                                     stage.time);
        env_drag_ = {};
        dragging = false;
    }

    // Curve + draggable points, from the preview while dragging and
    // the live stages otherwise.
    const std::vector<ntp::EnvelopeStage>& stages = dragging ? env_drag_.preview : live_stages;
    const double total = dragging ? env_drag_.total : axis_total(stages);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(origin, ImVec2{origin.x + width, origin.y + height},
                  ImGui::GetColorU32(ImGuiCol_Border));
    ImVec2 from{origin.x, origin.y + height};
    for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
        const ImVec2 to = stage_point(stages, i, total);
        draw->AddLine(from, to, ImGui::GetColorU32(theme.primary), 2.0F);
        from = to;
    }
    draw->AddLine(from, ImVec2{origin.x + width, origin.y + height},
                  ImGui::GetColorU32(theme.primary_dim), 2.0F);
    for (int i = 0; i < static_cast<int>(stages.size()); ++i) {
        const bool held = dragging && i == env_drag_.stage;
        draw->AddCircleFilled(stage_point(stages, i, total), held ? 4.5F : 3.0F,
                              ImGui::GetColorU32(held ? theme.text : theme.primary));
    }
    if (ImGui::IsItemHovered() || dragging) {
        const ntp::EnvelopeStage& stage =
            stages[static_cast<std::size_t>(dragging ? env_drag_.stage : 0)];
        ImGui::SetTooltip("%s%.0f ms -> %.2f", dragging ? "" : "drag points | ",
                          stage.time * 1000.0, stage.target);
    }
    ImGui::PopID();
}

void NtpUi::draw_auto_panel(plugins::NtpInstance& instance, const ntp::Manifest& manifest) {
    // Grouped sliders straight from the param list — the universal
    // fallback (shared conceptually with external plugin hosting).
    std::string current_group;
    for (const ntp::ParamDef& def : manifest.params) {
        if (def.group != current_group) {
            current_group = def.group;
            if (!current_group.empty()) {
                ImGui::SeparatorText(current_group.c_str());
            }
        }
        draw_param_widget(instance, def, false, 0.0F);
    }
    if (manifest.params.empty()) {
        ImGui::TextDisabled("no parameters");
    }
}

void NtpUi::draw_slot_picker(app::ProjectSession& session, const std::string& workspace_id,
                             plugins::NtpInstance& instance, const Theme& theme) {
    // Collect the manifest's user-assignable slots (plugin-wide unique
    // by the loader's validation; first zone wins the label).
    struct SlotInfo {
        const ntp::SampleZone* zone = nullptr;
    };

    std::vector<SlotInfo> slots;
    for (const ntp::DspNode& node : instance.manifest().graph.nodes) {
        for (const ntp::SampleZone& zone : node.zones) {
            if (zone.user_assignable && !zone.slot_id.empty()) {
                slots.push_back({.zone = &zone});
            }
        }
    }
    if (slots.empty()) {
        return;
    }

    ImGui::SeparatorText("SAMPLE SLOTS");
    const auto& overrides = instance.slot_overrides();
    for (const SlotInfo& info : slots) {
        const ntp::SampleZone& zone = *info.zone;
        ImGui::PushID(zone.slot_id.c_str());
        const std::string& label = zone.slot_label.empty() ? zone.slot_id : zone.slot_label;
        // Current assignment: user file + short content hash, or the
        // baked fallback.
        const auto it = overrides.find(zone.slot_id);
        if (it != overrides.end()) {
            // "sha256:" + 8 hex chars is enough to eyeball identity.
            const std::string short_hash =
                it->second.hash.size() > 15 ? it->second.hash.substr(0, 15) : it->second.hash;
            ImGui::Text("%s: %s", label.c_str(), it->second.name.c_str());
            ImGui::SameLine();
            ImGui::TextColored(theme.text_dim, "(%s)", short_hash.c_str());
        } else {
            ImGui::Text("%s:", label.c_str());
            ImGui::SameLine();
            ImGui::TextColored(theme.text_dim, "fallback: %s", zone.fallback_file.c_str());
        }

        SlotPickerState& state = slot_state_[workspace_id + "/" + zone.slot_id];
        ImGui::SetNextItemWidth(220.0F);
        ImGui::InputTextWithHint("##path", "path to wav/ogg/mp3", state.path.data(),
                                 state.path.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("LOAD") && state.path[0] != '\0') {
            state.status =
                session.set_plugin_sample_override(workspace_id, zone.slot_id, state.path.data())
                    ? ""
                    : session.error();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(it == overrides.end());
        if (ImGui::SmallButton("CLEAR")) {
            session.clear_plugin_sample_override(workspace_id, zone.slot_id);
            state.status.clear();
        }
        ImGui::EndDisabled();
        if (!state.status.empty()) {
            ImGui::TextColored(theme.text_dim, "%s", state.status.c_str());
        }
        ImGui::PopID();
    }
}

void NtpUi::draw(app::ProjectSession& session, const std::string& workspace_id,
                 const Theme& theme) {
    plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
    if (instance == nullptr) {
        ImGui::TextDisabled("instance missing");
        return;
    }
    const ntp::Manifest& manifest = instance->manifest();
    const plugins::LoadedNtpPlugin* plugin = session.plugin_registry().find(manifest.id);
    if (plugin == nullptr) {
        return;
    }

    // Trust surface (Plan_PostV1/10): an archive with native stages
    // executes code, unlike pure-data NTP — the marker shows wherever
    // the plugin UI shows, never silently.
    if (plugin->executes_native_code) {
        ImGui::TextColored(theme.primary, "[native code]");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("this plugin executes native code shipped in its archive\n"
                              "load it only if you trust the author");
        }
    }

    // Presets: factory (manifest), project (PPRS), library (config
    // dir), plus save-to-library of the current parameter values.
    ImGui::SetNextItemWidth(150.0F);
    if (ImGui::BeginCombo("##preset", "preset...")) {
        for (const ntp::Preset& preset : manifest.presets) {
            if (ImGui::Selectable(preset.name.c_str())) {
                instance->apply_preset(preset);
            }
        }
        auto apply_user = [&](const plugins::UserPreset& preset) {
            ntp::Preset converted;
            converted.name = preset.name;
            converted.values = preset.values;
            instance->apply_preset(converted);
        };
        if (const std::vector<plugins::UserPreset>* project =
                session.preset_bank().project_presets(workspace_id)) {
            ImGui::SeparatorText("project");
            for (const plugins::UserPreset& preset : *project) {
                if (ImGui::Selectable((preset.name + "##proj").c_str())) {
                    apply_user(preset);
                }
            }
        }
        const std::vector<plugins::UserPreset> library = session.preset_bank().library(manifest.id);
        if (!library.empty()) {
            ImGui::SeparatorText("library");
            for (const plugins::UserPreset& preset : library) {
                if (ImGui::Selectable((preset.name + "##lib").c_str())) {
                    apply_user(preset);
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    static std::array<char, 64> preset_name{};
    ImGui::SetNextItemWidth(90.0F);
    ImGui::InputTextWithHint("##pname", "name", preset_name.data(), preset_name.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("save") && preset_name[0] != '\0') {
        std::vector<std::pair<std::string, double>> values;
        values.reserve(manifest.params.size());
        for (const ntp::ParamDef& def : manifest.params) {
            values.emplace_back(def.key, static_cast<double>(instance->param_value(def.key)));
        }
        session.preset_bank().save_to_library(manifest.id, preset_name.data(), values);
        preset_name[0] = '\0';
    }

    if (manifest.ui.controls.empty()) {
        if (manifest.ui.had_webview) {
            ImGui::TextDisabled("webview UI: showing parameter panel");
        }
        draw_auto_panel(*instance, manifest);
        draw_slot_picker(session, workspace_id, *instance, theme);
        return;
    }
    for (const ntp::UiControl& control : manifest.ui.controls) {
        draw_control(session, workspace_id, *instance, manifest, *plugin, control, theme);
    }
    draw_slot_picker(session, workspace_id, *instance, theme);
}

} // namespace nt::ui
