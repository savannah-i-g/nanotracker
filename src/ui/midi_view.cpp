#include "ui/midi_view.h"

#include <imgui.h>
#include <string>
#include <vector>

namespace nt::ui {

MidiView::MidiView(midi::MidiInput& input, midi::MidiOutputPort& output,
                   midi::MidiOutThread& out_thread, midi::MidiLearn& learn)
    : input_(input), output_(output), out_thread_(out_thread), learn_(learn) {}

void MidiView::drain_input(app::ProjectSession& session) {
    midi::MidiEvent event;
    int budget = 128; // bounded per frame
    while (budget-- > 0 && input_.poll(event)) {
        switch (event.type) {
        case midi::MidiEvent::Type::kNoteOn: {
            const int slot = live_instrument_;
            const auto& table = session.project().instrument_table;
            const bool is_plugin = slot >= 1 && slot <= static_cast<int>(table.size()) &&
                                   table[static_cast<std::size_t>(slot - 1)].type !=
                                       engine::InstrumentSourceType::kSample;
            if (is_plugin) {
                session.preview_plugin_note(slot, event.data1,
                                            static_cast<float>(event.data2) / 127.0F);
            } else {
                session.preview_note(0, slot, event.data1 - 11);
            }
            last_note_ = event.data1;
            break;
        }
        case midi::MidiEvent::Type::kNoteOff:
            if (last_note_ == event.data1) {
                session.preview_plugin_release();
                last_note_ = -1;
            }
            break;
        case midi::MidiEvent::Type::kControlChange:
            learn_.handle_cc(session, event.channel, event.data1, event.data2);
            break;
        case midi::MidiEvent::Type::kOther:
            break;
        }
    }
}

void MidiView::draw(app::ProjectSession& session, const Theme& theme) {
    (void)theme;
    drain_input(session);

    ImGui::SetNextWindowPos(ImVec2{820, 60}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{360, 0}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("midi")) {
        ImGui::End();
        return;
    }

    // ── Devices ──────────────────────────────────────────────────────
    const std::vector<std::string> inputs = input_.port_names();
    if (ImGui::BeginCombo("input", input_.is_open() ? "(open)" : "select...")) {
        for (unsigned i = 0; i < inputs.size(); ++i) {
            if (ImGui::Selectable(inputs[i].c_str())) {
                std::string error;
                input_.close_port();
                input_.open_port(i, error);
            }
        }
        ImGui::EndCombo();
    }
    const std::vector<std::string> outputs = output_.port_names();
    if (ImGui::BeginCombo("output", output_.is_open() ? "(open)" : "select...")) {
        for (unsigned i = 0; i < outputs.size(); ++i) {
            if (ImGui::Selectable(outputs[i].c_str())) {
                std::string error;
                output_.close_port();
                output_.open_port(i, error);
            }
        }
        ImGui::EndCombo();
    }
    if (inputs.empty() && outputs.empty()) {
        ImGui::TextDisabled("no MIDI ports (backend or devices missing)");
    }

    bool clock = out_thread_.clock_enabled();
    if (ImGui::Checkbox("clock out (24 PPQN)", &clock)) {
        out_thread_.set_clock_enabled(clock);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%llu ticks",
                        static_cast<unsigned long long>(out_thread_.clock_ticks_sent()));

    ImGui::SetNextItemWidth(90.0F);
    ImGui::InputInt("live instrument", &live_instrument_);
    live_instrument_ = std::clamp(live_instrument_, 1, engine::kMaxSamples);

    // ── Learn ────────────────────────────────────────────────────────
    ImGui::SeparatorText("MIDI learn");

    // Learnable targets: every plugin node's parameters.
    struct Target {
        std::string label;
        std::string kind;
        std::string workspace_id;
        std::string param;
    };

    std::vector<Target> targets;
    for (const graph::Node& node : session.workspace().nodes()) {
        if (node.kind != graph::NodeKind::kPlugin) {
            continue;
        }
        if (plugins::NtpInstance* ntp = session.plugin_instance(node.workspace_id)) {
            for (const ntp::ParamDef& def : ntp->manifest().params) {
                targets.push_back(
                    {node.display_name + " / " + def.label, "ntp", node.workspace_id, def.key});
            }
        } else if (ext::ClapPlugin* clap = session.clap_instance(node.workspace_id)) {
            for (const ext::ClapParamInfo& param : clap->params()) {
                targets.push_back({node.display_name + " / " + param.name, "clap",
                                   node.workspace_id, std::to_string(param.id)});
            }
        } else if (ext::Vst3Plugin* vst3 = session.vst3_instance(node.workspace_id)) {
            int listed = 0;
            for (const ext::Vst3ParamInfo& param : vst3->params()) {
                if (!param.automatable || listed++ >= 64) {
                    continue;
                }
                targets.push_back({node.display_name + " / " + param.title, "vst3",
                                   node.workspace_id, std::to_string(param.id)});
            }
        }
    }
    learn_target_ = std::clamp(learn_target_, 0, std::max(0, static_cast<int>(targets.size()) - 1));
    if (targets.empty()) {
        ImGui::TextDisabled("no learnable parameters (add a plugin node)");
    } else {
        ImGui::SetNextItemWidth(230.0F);
        if (ImGui::BeginCombo("##target",
                              targets[static_cast<std::size_t>(learn_target_)].label.c_str())) {
            for (int t = 0; t < static_cast<int>(targets.size()); ++t) {
                if (ImGui::Selectable(targets[static_cast<std::size_t>(t)].label.c_str(),
                                      t == learn_target_)) {
                    learn_target_ = t;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (learn_.armed()) {
            if (ImGui::SmallButton("waiting for CC... cancel")) {
                learn_.cancel();
            }
        } else if (ImGui::SmallButton("learn")) {
            const Target& target = targets[static_cast<std::size_t>(learn_target_)];
            learn_.arm(target.kind, target.workspace_id, target.param);
        }
    }
    for (std::size_t m = 0; m < learn_.mappings().size(); ++m) {
        const midi::MidiMapping& mapping = learn_.mappings()[m];
        ImGui::PushID(static_cast<int>(m));
        ImGui::TextDisabled("ch%d cc%d -> %s %s", mapping.channel + 1, mapping.cc,
                            mapping.workspace_id.c_str(), mapping.param.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            learn_.remove_mapping(m);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace nt::ui
