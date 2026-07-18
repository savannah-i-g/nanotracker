#include "ui/instrument_table_view.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

constexpr std::array<const char*, 3> kSourceTypeNames = {"sample", "plugin", "workspace"};

// Combo over the NTP catalogue; returns true when the entry changed.
bool plugin_source_combo(app::ProjectSession& session, engine::InstrumentTableEntry& entry) {
    const auto& catalogue = session.plugin_registry().all();
    const char* current = entry.plugin_id.empty() ? "(none)" : entry.plugin_id.c_str();
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::BeginCombo("##plug", current)) {
        for (const auto& plugin : catalogue) {
            const std::string& id = plugin->manifest.id;
            const bool selected = id == entry.plugin_id;
            if (ImGui::Selectable(plugin->manifest.name.c_str(), selected)) {
                entry.plugin_id = id;
                entry.workspace_id.clear();
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Combo over live plugin workspace nodes (NTP/CLAP/VST3 instances all
// bind through the same workspace-id path).
bool workspace_source_combo(app::ProjectSession& session, engine::InstrumentTableEntry& entry) {
    const char* current = entry.workspace_id.empty() ? "(none)" : entry.workspace_id.c_str();
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::BeginCombo("##wsp", current)) {
        for (const graph::Node& node : session.workspace().nodes()) {
            if (node.kind != graph::NodeKind::kPlugin) {
                continue;
            }
            const bool selected = node.workspace_id == entry.workspace_id;
            std::array<char, 96> label{};
            std::snprintf(label.data(), label.size(), "%s (%s)", node.display_name.c_str(),
                          node.workspace_id.c_str());
            if (ImGui::Selectable(label.data(), selected)) {
                entry.workspace_id = node.workspace_id;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

void InstrumentTableView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(620, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("INSTRUMENTS")) {
        engine::TrackerProject& project = session.project();
        const int channels = project.channels;

        ImGui::TextColored(theme.text_dim,
                           "slot → source; ▣ = bound tracks (instrument-free note entry)");
        ImGui::Separator();

        if (ImGui::BeginTable("instr", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 44.0F);
            ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 96.0F);
            ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthFixed, 170.0F);
            ImGui::TableSetupColumn("bound tracks");
            ImGui::TableHeadersRow();

            for (int slot = 1; slot <= engine::kMaxSamples; ++slot) {
                ImGui::TableNextRow();
                ImGui::PushID(slot);

                engine::InstrumentTableEntry entry;
                const bool has_entry = slot <= static_cast<int>(project.instrument_table.size());
                if (has_entry) {
                    entry = project.instrument_table[static_cast<std::size_t>(slot - 1)];
                } else {
                    entry.sample_id = slot; // legacy direct mapping
                }

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(theme.primary, "%02X", slot);

                ImGui::TableSetColumnIndex(1);
                int type_index = static_cast<int>(entry.type);
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::Combo("##type", &type_index, kSourceTypeNames.data(),
                                 static_cast<int>(kSourceTypeNames.size()))) {
                    entry.type = static_cast<engine::InstrumentSourceType>(type_index);
                    session.set_instrument_entry(slot, entry);
                }

                ImGui::TableSetColumnIndex(2);
                switch (entry.type) {
                case engine::InstrumentSourceType::kSample: {
                    int sample_id = entry.sample_id;
                    ImGui::SetNextItemWidth(-1.0F);
                    if (ImGui::InputInt("##smp", &sample_id, 0)) {
                        entry.sample_id = std::clamp(sample_id, 0, engine::kMaxSamples);
                        session.set_instrument_entry(slot, entry);
                    }
                    break;
                }
                case engine::InstrumentSourceType::kPlugin:
                    if (plugin_source_combo(session, entry)) {
                        session.set_instrument_entry(slot, entry);
                    }
                    break;
                case engine::InstrumentSourceType::kWorkspace:
                    if (workspace_source_combo(session, entry)) {
                        session.set_instrument_entry(slot, entry);
                    }
                    break;
                }

                ImGui::TableSetColumnIndex(3);
                bool bound_changed = false;
                for (int ch = 0; ch < channels; ++ch) {
                    const bool bound =
                        std::find(entry.bound_tracks.begin(), entry.bound_tracks.end(), ch) !=
                        entry.bound_tracks.end();
                    std::array<char, 8> label{};
                    std::snprintf(label.data(), label.size(), "%d", ch + 1);
                    if (ch > 0) {
                        ImGui::SameLine(0.0F, 3.0F);
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, bound ? theme.primary : theme.text_dim);
                    ImGui::PushID(ch);
                    if (ImGui::SmallButton(label.data())) {
                        if (bound) {
                            entry.bound_tracks.erase(std::remove(entry.bound_tracks.begin(),
                                                                 entry.bound_tracks.end(), ch),
                                                     entry.bound_tracks.end());
                        } else {
                            entry.bound_tracks.push_back(ch);
                            std::sort(entry.bound_tracks.begin(), entry.bound_tracks.end());
                        }
                        bound_changed = true;
                    }
                    ImGui::PopID();
                    ImGui::PopStyleColor();
                }
                if (bound_changed) {
                    session.set_instrument_entry(slot, entry);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

} // namespace nt::ui
