#include "ui/midi_view.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <vector>

namespace nt::ui {

MidiView::MidiView(midi::MidiInput& input, midi::MidiOutputPort& output,
                   midi::MidiOutThread& out_thread, midi::MidiLearn& learn, app::MidiRecord& record)
    : input_(input), output_(output), out_thread_(out_thread), learn_(learn), record_(record) {}

void MidiView::drain_input(app::ProjectSession& session) {
    midi::MidiEvent event;
    int budget = 128; // bounded per frame
    while (budget-- > 0 && input_.poll(event)) {
        switch (event.type) {
        case midi::MidiEvent::Type::kNoteOn:
            // Learn armed = learn wins (header): notes are dropped so
            // binding gestures never preview or record.
            if (!learn_.armed()) {
                record_.on_device_note_on(session, event.data1, event.data2);
            }
            break;
        case midi::MidiEvent::Type::kNoteOff:
            if (!learn_.armed()) {
                record_.on_device_note_off(session, event.data1);
            }
            break;
        case midi::MidiEvent::Type::kControlChange:
            // CCs always belong to learn/mappings; the record
            // controller never sees them (notes-only, web parity).
            learn_.handle_cc(session, event.channel, event.data1, event.data2);
            break;
        case midi::MidiEvent::Type::kOther:
            break;
        }
    }
}

void MidiView::draw_input_mode(app::ProjectSession& session, const Theme& theme) {
    ImGui::SeparatorText("input mode");

    // Web TrackerMidiPanel modes plus an explicit off (the web
    // disabled input by closing its panel).
    static constexpr std::array<const char*, 4> kModeLabels = {"off", "preview", "enter", "record"};
    int mode = static_cast<int>(record_.mode());
    for (int m = 0; m < static_cast<int>(kModeLabels.size()); ++m) {
        if (m > 0) {
            ImGui::SameLine();
        }
        ImGui::RadioButton(kModeLabels[static_cast<std::size_t>(m)], &mode, m);
    }
    record_.set_mode(static_cast<app::MidiInputMode>(mode));
    switch (record_.mode()) {
    case app::MidiInputMode::kOff:
        ImGui::TextDisabled("inbound notes ignored");
        break;
    case app::MidiInputMode::kPreview:
        ImGui::TextDisabled("plays the preview slot - no pattern writes");
        break;
    case app::MidiInputMode::kEnter:
        ImGui::TextDisabled("writes notes at the pattern cursor, step by step");
        break;
    case app::MidiInputMode::kRecord:
        ImGui::TextDisabled("writes notes at the playhead row during playback");
        break;
    }

    int slot = record_.preview_slot();
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::InputInt("preview slot", &slot)) {
        record_.set_preview_slot(std::clamp(slot, 1, engine::kMaxSamples));
    }

    // Armed channel: the record target (and preview channel). Count
    // from the open project.
    const int channels = session.project().channels;
    int armed = std::clamp(record_.armed_channel(), 0, channels - 1);
    std::array<char, 16> armed_label{};
    std::snprintf(armed_label.data(), armed_label.size(), "CH%d", armed + 1);
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::BeginCombo("armed channel", armed_label.data())) {
        for (int ch = 0; ch < channels; ++ch) {
            std::array<char, 16> label{};
            std::snprintf(label.data(), label.size(), "CH%d", ch + 1);
            if (ImGui::Selectable(label.data(), ch == armed)) {
                armed = ch;
            }
        }
        ImGui::EndCombo();
    }
    record_.set_armed_channel(armed);

    bool vel_to_vol = record_.velocity_to_volume();
    if (ImGui::Checkbox("velocity to volume", &vel_to_vol)) {
        record_.set_velocity_to_volume(vel_to_vol);
    }

    if (record_.mode() == app::MidiInputMode::kRecord) {
        if (session.playing()) {
            // Live indicator: armed and rolling.
            ImGui::TextColored(ImVec4(1.0F, 0.30F, 0.25F, 1.0F), "REC * writing to CH%d",
                               record_.armed_channel() + 1);
        } else {
            ImGui::TextColored(theme.primary_dim, "start playback (F5) to record");
        }
    }
}

void MidiView::draw(app::ProjectSession& session, const Theme& theme) {
    // Note: device intake is drained unconditionally from the main loop
    // (drain_input), not here — hiding this window must not pause it.
    ImGui::SetNextWindowPos(ImVec2{820, 60}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{360, 0}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MIDI")) {
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

    // ── Input mode (app/midi_record) ─────────────────────────────────
    draw_input_mode(session, theme);

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
