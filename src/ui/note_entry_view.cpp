#include "ui/note_entry_view.h"

#include "engine/tracker_engine.h"
#include "engine/tracker_types.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

// Black keys of a tracker note (1 = C-0): semitones C#, D#, F#, G#, A#.
bool is_black_key(int note) {
    switch ((note - 1) % 12) {
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

} // namespace

void NoteEntryView::draw(app::ProjectSession& session, PatternView& pattern,
                         const app::MidiRecord& record, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2{470, 250}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("NOTE ENTRY")) {
        ImGui::End();
        return;
    }

    // ── Entry state (edits ui::PatternView's shared members) ─────────
    int octave = pattern.octave();
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::InputInt("octave", &octave)) {
        pattern.set_octave(octave);
    }
    ImGui::SetItemTooltip("note-key octave; F1/F2 in the grid also change it");
    ImGui::SameLine();
    int step = pattern.edit_step();
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::InputInt("edit step", &step)) {
        pattern.set_edit_step(step);
    }
    ImGui::SetItemTooltip("rows the cursor advances per note (0 holds)");

    int slot = pattern.entry_slot();
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::InputInt("SMP slot", &slot)) {
        pattern.set_entry_slot(slot);
    }
    ImGui::SetItemTooltip("instrument slot written on entry (bound channels resolve at playback)");
    ImGui::SameLine();
    // Default volume: the checkbox gates a 0..64 stamp; unchecked leaves
    // the volume column untouched (-1) — byte-for-byte classic typing.
    bool stamp = pattern.default_volume() >= 0;
    if (ImGui::Checkbox("default vol", &stamp)) {
        pattern.set_default_volume(stamp ? 0x40 : -1);
    }
    ImGui::SetItemTooltip("off = leave the volume column, as typing does");
    if (stamp) {
        ImGui::SameLine();
        int volume = pattern.default_volume();
        ImGui::SetNextItemWidth(150.0F);
        if (ImGui::SliderInt("##vol", &volume, 0, 0x40, "vol %02X")) {
            pattern.set_default_volume(volume);
        }
    }

    ImGui::Separator();

    // ── Readouts: last note + active entry mode ──────────────────────
    const int last = pattern.last_note();
    std::array<char, 8> last_name{};
    engine::note_name(last, last_name.data());
    ImGui::TextUnformatted("last note");
    ImGui::SameLine();
    ImGui::TextColored(last > 0 ? theme.primary : theme.text_dim, "%s",
                       last > 0 ? last_name.data() : "---");
    ImGui::SameLine();
    ImGui::TextDisabled(" | ");
    ImGui::SameLine();
    // Keyboard entry (typing / on-screen keys) is always live; MIDI step
    // enter and live record overlay it when armed (app::MidiRecord).
    const char* mode_label = "keyboard";
    switch (record.mode()) {
    case app::MidiInputMode::kEnter:
        mode_label = "keyboard + MIDI enter";
        break;
    case app::MidiInputMode::kRecord:
        mode_label = "keyboard + MIDI record";
        break;
    case app::MidiInputMode::kOff:
    case app::MidiInputMode::kPreview:
        break;
    }
    ImGui::TextUnformatted("entry");
    ImGui::SameLine();
    ImGui::TextColored(theme.primary_dim, "%s", mode_label);

    ImGui::Separator();

    // ── On-screen keyboard ───────────────────────────────────────────
    // Spans keyboard_octaves_ octaves up from the entry octave. A click
    // enters the note at the pattern cursor through the identical path a
    // keystroke uses, then advances the cursor and auditions.
    ImGui::TextDisabled("click a key to enter a note at the pattern cursor");
    ImGui::BeginChild("keys", ImVec2{0, 72.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const int base_note = std::clamp((pattern.octave() * 12) + 1, 1, engine::kMaxNote);
    const int key_count = keyboard_octaves_ * 12;
    for (int k = 0; k < key_count; ++k) {
        const int note = base_note + k;
        if (note > engine::kMaxNote) {
            break;
        }
        if (k > 0) {
            ImGui::SameLine(0.0F, 2.0F);
        }
        std::array<char, 8> name{};
        engine::note_name(note, name.data());
        std::array<char, 16> label{};
        std::snprintf(label.data(), label.size(), "%s##nk%d", name.data(), k);
        const bool black = is_black_key(note);
        if (black) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1F, 0.08F, 0.05F, 1.0F});
        }
        ImGui::Button(label.data(), ImVec2{30.0F, black ? 34.0F : 48.0F});
        if (black) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemActivated()) {
            // EXACTLY the keyboard-entry path: cursor write (+ audition)
            // then step advance. default_volume() (-1 by default) keeps
            // the click identical to typing the same note.
            app::enter_note_cell(session, pattern.step_pattern(), pattern.step_row(),
                                 pattern.step_channel(), note, pattern.step_entry_slot(),
                                 pattern.default_volume(), true);
            pattern.set_last_note(note);
            pattern.step_advance();
            keyboard_note_down_ = note;
        }
        if (ImGui::IsItemDeactivated() && keyboard_note_down_ == note) {
            session.preview_plugin_release(); // no-op for sample previews
            keyboard_note_down_ = -1;
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace nt::ui
