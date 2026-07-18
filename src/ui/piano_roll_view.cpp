#include "ui/piano_roll_view.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

constexpr int kPitchRows = 36; // three octaves visible
constexpr float kRowHeight = 12.0F;
constexpr float kTickWidth = 3.0F;

bool is_black_key(int pitch) {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

const char* note_name(int pitch) {
    static const std::array<const char*, 12> kNames = {"C-", "C#", "D-", "D#", "E-", "F-",
                                                       "F#", "G-", "G#", "A-", "A#", "B-"};
    return kNames[static_cast<std::size_t>(pitch % 12)];
}

} // namespace

void PianoRollView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowPos(ImVec2{60, 60}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{720, 560}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("piano roll")) {
        ImGui::End();
        return;
    }
    const engine::TrackerProject& project = session.project();
    const int pattern_index = 0; // follows the pattern editor later
    channel_ = std::clamp(channel_, 0, project.channels - 1);

    ImGui::SetNextItemWidth(90.0F);
    ImGui::SliderInt("channel", &channel_, 0, project.channels - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    ImGui::SliderInt("layer", &layer_, 0, engine::kMaxSeqLayersPerChannel - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0F);
    ImGui::SliderInt("octave", &base_pitch_, 24, 72, "base %d");

    engine::SequenceLayer* layer = session.seq_layer(pattern_index, channel_, layer_, false);
    int instrument = layer != nullptr ? layer->instrument : 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0F);
    if (ImGui::InputInt("ins", &instrument)) {
        session.seq_set_layer_instrument(pattern_index, channel_, layer_,
                                         std::clamp(instrument, 0, engine::kMaxSamples));
        layer = session.seq_layer(pattern_index, channel_, layer_, false);
    }
    bool enabled = layer == nullptr || layer->enabled;
    ImGui::SameLine();
    if (ImGui::Checkbox("on", &enabled)) {
        session.seq_set_layer_enabled(pattern_index, channel_, layer_, enabled);
    }

    // ── Note grid ────────────────────────────────────────────────────
    const int rows = project.rows_per_pattern;
    const int ticks_per_row = std::max(1, project.speed);
    const int total_ticks = rows * ticks_per_row;
    const float grid_width = static_cast<float>(total_ticks) * kTickWidth;
    const float grid_height = kPitchRows * kRowHeight;

    ImGui::BeginChild("grid", ImVec2{0, grid_height + 16.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Row shading + line grid.
    for (int p = 0; p < kPitchRows; ++p) {
        const int pitch = base_pitch_ + (kPitchRows - 1 - p);
        const float y = origin.y + (static_cast<float>(p) * kRowHeight);
        if (is_black_key(pitch)) {
            draw->AddRectFilled(ImVec2{origin.x, y}, ImVec2{origin.x + grid_width, y + kRowHeight},
                                ImGui::GetColorU32(theme.background_alt));
        }
    }
    for (int r = 0; r <= rows; ++r) {
        const float x = origin.x + (static_cast<float>(r * ticks_per_row) * kTickWidth);
        const bool major = r % 4 == 0;
        draw->AddLine(ImVec2{x, origin.y}, ImVec2{x, origin.y + grid_height},
                      ImGui::GetColorU32(major ? theme.border : theme.background_alt));
    }
    draw->AddRect(origin, ImVec2{origin.x + grid_width, origin.y + grid_height},
                  ImGui::GetColorU32(theme.border));

    // Existing notes.
    int hovered_note = -1;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (layer != nullptr) {
        for (std::size_t n = 0; n < layer->notes.size(); ++n) {
            const engine::SequenceNote& note = layer->notes[n];
            const int row_offset = note.pitch - base_pitch_;
            if (row_offset < 0 || row_offset >= kPitchRows) {
                continue;
            }
            const float x0 = origin.x + (static_cast<float>(note.start_tick) * kTickWidth);
            const float x1 =
                origin.x + (static_cast<float>(note.start_tick + note.duration_ticks) * kTickWidth);
            const float y0 =
                origin.y + (static_cast<float>(kPitchRows - 1 - row_offset) * kRowHeight);
            draw->AddRectFilled(ImVec2{x0 + 1, y0 + 1}, ImVec2{x1 - 1, y0 + kRowHeight - 1},
                                ImGui::GetColorU32(theme.primary));
            if (mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 && mouse.y < y0 + kRowHeight) {
                hovered_note = static_cast<int>(n);
            }
        }
    }

    // Interaction: click empty = add a one-row note; right-click = delete.
    ImGui::InvisibleButton("gridhit", ImVec2{grid_width, grid_height});
    if (ImGui::IsItemHovered()) {
        const int tick =
            std::clamp(static_cast<int>((mouse.x - origin.x) / kTickWidth), 0, total_ticks - 1);
        const int row_tick = (tick / ticks_per_row) * ticks_per_row; // snap to row
        const int pitch =
            base_pitch_ +
            (kPitchRows - 1 -
             std::clamp(static_cast<int>((mouse.y - origin.y) / kRowHeight), 0, kPitchRows - 1));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_note < 0) {
            session.seq_add_note(pattern_index, channel_, layer_,
                                 {.pitch = pitch,
                                  .start_tick = row_tick,
                                  .duration_ticks = ticks_per_row,
                                  .velocity = 100});
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_note >= 0) {
            session.seq_remove_note(pattern_index, channel_, layer_, hovered_note);
        }
        ImGui::SetTooltip("%s%d tick %d", note_name(pitch), (pitch / 12) - 1, row_tick);
    }
    ImGui::EndChild();

    // ── On-screen keyboard (two octaves, audition path) ──────────────
    const int kb_base = base_pitch_ + 12;
    ImGui::TextDisabled("keyboard");
    for (int k = 0; k < 24; ++k) {
        const int pitch = kb_base + k;
        if (k > 0) {
            ImGui::SameLine(0.0F, 2.0F);
        }
        std::array<char, 12> label{};
        std::snprintf(label.data(), label.size(), "%s%d##kb%d", note_name(pitch), (pitch / 12) - 1,
                      k);
        const bool black = is_black_key(pitch);
        if (black) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1F, 0.08F, 0.05F, 1.0F});
        }
        ImGui::Button(label.data(), ImVec2{26.0F, black ? 30.0F : 42.0F});
        if (black) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemActivated()) {
            const int slot = layer != nullptr && layer->instrument > 0 ? layer->instrument : 1;
            // Sample instruments audition through the channel preview;
            // plugin instruments through the note command path.
            const auto& table = project.instrument_table;
            const bool is_plugin = slot >= 1 && slot <= static_cast<int>(table.size()) &&
                                   table[static_cast<std::size_t>(slot - 1)].type !=
                                       engine::InstrumentSourceType::kSample;
            if (is_plugin) {
                session.preview_plugin_note(slot, pitch, 0.9F);
            } else {
                session.preview_note(channel_, slot, pitch - 11); // MIDI → tracker note
            }
            keyboard_note_down_ = pitch;
        }
        if (ImGui::IsItemDeactivated() && keyboard_note_down_ == pitch) {
            session.preview_plugin_release();
            keyboard_note_down_ = -1;
        }
    }

    ImGui::End();
}

} // namespace nt::ui
