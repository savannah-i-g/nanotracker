#include "ui/fx_mixer_view.h"

#include "audio/fx_chain.h"

#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

void FxMixerView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(680, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("FX MIXER")) {
        engine::TrackerProject& project = session.project();
        auto& strips = project.fx_mixer.channels;

        if (ImGui::Button("+ CHANNEL")) {
            session.add_fx_channel();
        }
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim,
                           "structural edits stop the transport and rebuild the rack");
        ImGui::Separator();

        for (int ci = 0; ci < static_cast<int>(strips.size()); ++ci) {
            engine::FxChannelStrip& strip = strips[static_cast<std::size_t>(ci)];
            ImGui::PushID(ci);

            ImGui::TextColored(theme.primary, "%s", strip.name.c_str());
            ImGui::SameLine();
            bool enabled = strip.enabled;
            if (ImGui::Checkbox("on", &enabled)) {
                session.set_fx_strip(ci, strip.volume, strip.pan, enabled);
            }
            ImGui::SameLine();
            int volume = strip.volume;
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::SliderInt("vol", &volume, 0, 100)) {
                session.set_fx_strip(ci, volume, strip.pan, strip.enabled);
            }
            ImGui::SameLine();
            int pan = strip.pan;
            ImGui::SetNextItemWidth(120.0F);
            if (ImGui::SliderInt("pan", &pan, -100, 100)) {
                session.set_fx_strip(ci, strip.volume, pan, strip.enabled);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                session.remove_fx_channel(ci);
                ImGui::PopID();
                break;
            }

            // Sends per tracker channel.
            for (int ch = 0; ch < project.channels; ++ch) {
                if (ch > 0) {
                    ImGui::SameLine();
                }
                float amount = ch < static_cast<int>(strip.tracker_sends.size())
                                   ? strip.tracker_sends[static_cast<std::size_t>(ch)]
                                   : 0.0F;
                std::array<char, 12> label{};
                std::snprintf(label.data(), label.size(), "s%d", ch + 1);
                ImGui::SetNextItemWidth(70.0F);
                ImGui::PushID(100 + ch);
                if (ImGui::SliderFloat(label.data(), &amount, 0.0F, 1.0F, "%.2f")) {
                    session.set_fx_send(ci, ch, amount);
                }
                ImGui::PopID();
            }

            // Module stack.
            for (int mi = 0; mi < static_cast<int>(strip.modules.size()); ++mi) {
                engine::FxModuleInstance& instance = strip.modules[static_cast<std::size_t>(mi)];
                const audio::FxModuleDef* def = audio::fx_module_by_id(instance.module_id);
                if (def == nullptr) {
                    continue;
                }
                ImGui::PushID(200 + mi);
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextColored(theme.text, "%s", def->name);
                ImGui::SameLine();
                if (ImGui::SmallButton("remove")) {
                    session.remove_fx_module(ci, mi);
                    ImGui::PopID();
                    break;
                }
                for (int pi = 0; pi < static_cast<int>(def->params.size()); ++pi) {
                    const audio::FxParamDef& param = def->params[static_cast<std::size_t>(pi)];
                    float value = pi < static_cast<int>(instance.params.size())
                                      ? instance.params[static_cast<std::size_t>(pi)].second
                                      : param.def;
                    ImGui::SetNextItemWidth(110.0F);
                    if (pi % 4 != 0) {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(pi);
                    if (ImGui::SliderFloat(param.label, &value, param.min, param.max, "%.2f")) {
                        session.set_fx_module_param(ci, mi, pi, value);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }

            // Add-module picker.
            ImGui::SetNextItemWidth(160.0F);
            if (ImGui::BeginCombo("##add", "+ MODULE")) {
                for (const audio::FxModuleDef& def : audio::fx_module_registry()) {
                    if (ImGui::Selectable(def.name)) {
                        session.add_fx_module(ci, def.id);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace nt::ui
