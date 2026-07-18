#include "ui/instrument_table_view.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

void InstrumentTableView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(520, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("INSTRUMENTS")) {
        engine::TrackerProject& project = session.project();
        const int channels = project.channels;

        ImGui::TextColored(theme.text_dim,
                           "slot → source; ▣ = bound tracks (instrument-free note entry)");
        ImGui::Separator();

        if (ImGui::BeginTable("instr", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 44.0F);
            ImGui::TableSetupColumn("sample", ImGuiTableColumnFlags_WidthFixed, 90.0F);
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
                int sample_id = entry.sample_id;
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::InputInt("##smp", &sample_id, 0)) {
                    entry.sample_id = std::clamp(sample_id, 0, engine::kMaxSamples);
                    session.set_instrument_entry(slot, entry);
                }

                ImGui::TableSetColumnIndex(2);
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
