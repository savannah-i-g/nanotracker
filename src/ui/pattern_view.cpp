#include "ui/pattern_view.h"

#include "app/pattern_ops.h"
#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

// Grid metrics: proportional to the font so DPI scaling holds; ratios
// match the web renderer's 12px-font layout (ROW_H 14, COL_W 115).
struct GridMetrics {
    float row_h;
    float row_num_w;
    float col_w;
    float header_h;
    float char_w;
};

GridMetrics metrics() {
    const float char_w = ImGui::CalcTextSize("0").x;
    GridMetrics m{};
    m.char_w = char_w;
    m.row_h = ImGui::GetTextLineHeight() + 2.0F;
    m.row_num_w = char_w * 3.5F;
    // Cell text: "C-4 01 40 000" = 13 chars + padding.
    m.col_w = char_w * 15.0F;
    m.header_h = m.row_h * 2.2F;
    return m;
}

ImU32 col(const ImVec4& v, float alpha_scale = 1.0F) {
    ImVec4 c = v;
    c.w *= alpha_scale;
    return ImGui::GetColorU32(c);
}

// QWERTY → note offset, mirroring the web KEY_NOTE_MAP: lower row =
// entry octave, upper row = octave above.
struct NoteKey {
    ImGuiKey key;
    int offset;
};

constexpr std::array<NoteKey, 24> kNoteKeys = {{
    {ImGuiKey_Z, 1},  {ImGuiKey_S, 2},  {ImGuiKey_X, 3},  {ImGuiKey_D, 4},  {ImGuiKey_C, 5},
    {ImGuiKey_V, 6},  {ImGuiKey_G, 7},  {ImGuiKey_B, 8},  {ImGuiKey_H, 9},  {ImGuiKey_N, 10},
    {ImGuiKey_J, 11}, {ImGuiKey_M, 12}, {ImGuiKey_Q, 13}, {ImGuiKey_2, 14}, {ImGuiKey_W, 15},
    {ImGuiKey_3, 16}, {ImGuiKey_E, 17}, {ImGuiKey_R, 18}, {ImGuiKey_5, 19}, {ImGuiKey_T, 20},
    {ImGuiKey_6, 21}, {ImGuiKey_Y, 22}, {ImGuiKey_7, 23}, {ImGuiKey_U, 24},
}};

// Hex entry keys 0-9 A-F.
constexpr std::array<std::pair<ImGuiKey, int>, 16> kHexKeys = {{
    {ImGuiKey_0, 0},
    {ImGuiKey_1, 1},
    {ImGuiKey_2, 2},
    {ImGuiKey_3, 3},
    {ImGuiKey_4, 4},
    {ImGuiKey_5, 5},
    {ImGuiKey_6, 6},
    {ImGuiKey_7, 7},
    {ImGuiKey_8, 8},
    {ImGuiKey_9, 9},
    {ImGuiKey_A, 10},
    {ImGuiKey_B, 11},
    {ImGuiKey_C, 12},
    {ImGuiKey_D, 13},
    {ImGuiKey_E, 14},
    {ImGuiKey_F, 15},
}};

// Cursor fields split the effect command from its parameter; block
// operations treat them as one sub-column (pattern_ops CellField).
app::CellField cell_field(int cursor_field) {
    if (cursor_field <= static_cast<int>(app::CellField::kVolume)) {
        return static_cast<app::CellField>(cursor_field);
    }
    return app::CellField::kEffect;
}

// Sub-column x bands inside a cell in character units of the drawn
// text layout "C-4 01 40 000"; the effect band runs to the cell edge.
// Must agree with the click hit-test bands below.
constexpr std::array<float, app::kCellFieldCount> kFieldLeft = {0.0F, 4.0F, 7.0F, 10.0F};
constexpr std::array<float, app::kCellFieldCount> kFieldRight = {4.0F, 7.0F, 10.0F, 15.0F};

} // namespace

void PatternView::advance_cursor_down(int rows) {
    // Edit step (0 holds the cursor); scroll follows for a step that
    // jumps past the visible band.
    cursor_.row = std::min(rows - 1, cursor_.row + std::max(0, edit_step_));
    while (cursor_.row >= scroll_row_ + visible_rows_) {
        ++scroll_row_;
    }
}

void PatternView::clamp_scroll(int rows, int visible_rows) {
    visible_rows_ = std::max(1, visible_rows);
    scroll_row_ = std::clamp(scroll_row_, 0, std::max(0, rows - 1));
    cursor_.row = std::clamp(cursor_.row, 0, std::max(0, rows - 1));
    if (cursor_.row < scroll_row_) {
        scroll_row_ = cursor_.row;
    }
    if (cursor_.row >= scroll_row_ + visible_rows_) {
        scroll_row_ = cursor_.row - visible_rows_ + 1;
    }
}

app::CellSelection PatternView::block_selection(int pattern_index) const {
    app::CellSelection sel;
    sel.pattern = pattern_index;
    sel.row0 = selection_.active ? selection_.row : cursor_.row;
    sel.row1 = cursor_.row;
    sel.channel0 = selection_.active ? selection_.channel : cursor_.channel;
    sel.channel1 = cursor_.channel;
    sel.field0 = cell_field(selection_.active ? selection_.field : cursor_.field);
    sel.field1 = cell_field(cursor_.field);
    return sel;
}

void PatternView::handle_keys(app::ProjectSession& session, const engine::TrackerPattern& pattern,
                              int pattern_index, int visible_rows) {
    const int rows = static_cast<int>(pattern.rows.size());
    const int channels = session.project().channels;
    const engine::TrackerProject& project = session.project();
    const ImGuiIO& io = ImGui::GetIO();

    // Transport + octave.
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (session.playing()) {
            session.stop();
        } else {
            session.play();
        }
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        session.stop();
        return;
    }
    if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        octave_ = std::max(0, octave_ - 1);
    }
    if (!io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        octave_ = std::min(7, octave_ + 1);
    }

    // Classic tracker transpose over the selection (or cursor cell):
    // Alt+F1/F2 by a semitone, Alt+F3/F4 by an octave.
    if (io.KeyAlt) {
        int semitones = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
            semitones = -1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            semitones = 1;
        } else if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
            semitones = -12;
        } else if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) {
            semitones = 12;
        }
        if (semitones != 0) {
            app::transpose_cells(session, block_selection(pattern_index), semitones);
            return;
        }
    }

    // Undo / redo.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        session.undo().undo();
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        session.undo().redo();
        return;
    }

    // Block clipboard + interpolate (app/pattern_ops); with no active
    // selection these act on the cursor cell as a 1×1 block.
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        clipboard_ = app::copy_cells(session, block_selection(pattern_index));
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) {
        clipboard_ = app::cut_cells(session, block_selection(pattern_index));
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        app::paste_cells(session, clipboard_, pattern_index, cursor_.row, cursor_.channel);
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        app::interpolate_cells(session, block_selection(pattern_index));
        return;
    }

    // Navigation. Shift-extended movement grows the selection from a
    // fixed anchor; unmodified movement collapses it, Escape drops it.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        selection_.active = false;
    }
    const auto track_selection = [this, &io] {
        if (!io.KeyShift) {
            selection_.active = false;
        } else if (!selection_.active) {
            selection_ = {cursor_.row, cursor_.channel, cursor_.field, true};
        }
    };
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        track_selection();
        cursor_.row = std::max(0, cursor_.row - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        track_selection();
        cursor_.row = std::min(rows - 1, cursor_.row + 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        track_selection();
        if (cursor_.field > 0) {
            --cursor_.field;
        } else if (cursor_.channel > 0) {
            --cursor_.channel;
            cursor_.field = 4;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        track_selection();
        if (cursor_.field < 4) {
            ++cursor_.field;
        } else if (cursor_.channel < channels - 1) {
            ++cursor_.channel;
            cursor_.field = 0;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        selection_.active = false; // shift here means reverse-tab
        const int step = io.KeyShift ? channels - 1 : 1;
        cursor_.channel = (cursor_.channel + step) % channels;
        cursor_.field = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        track_selection();
        cursor_.row = std::max(0, cursor_.row - 16);
        scroll_row_ = std::max(0, scroll_row_ - 16);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        track_selection();
        cursor_.row = std::min(rows - 1, cursor_.row + 16);
        scroll_row_ += 16;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        track_selection();
        cursor_.row = 0;
        scroll_row_ = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
        track_selection();
        cursor_.row = rows - 1;
        scroll_row_ = std::max(0, rows - visible_rows);
    }

    const engine::TrackerCell current =
        pattern
            .rows[static_cast<std::size_t>(cursor_.row)][static_cast<std::size_t>(cursor_.channel)];

    // Delete clears the active selection as a block (Ctrl forces the
    // block path for the cursor cell); without one it keeps the
    // classic per-field clear with cursor advance.
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        if (selection_.active || io.KeyCtrl) {
            app::clear_cells(session, block_selection(pattern_index));
            return;
        }
        engine::TrackerCell cell = current;
        switch (cursor_.field) {
        case 0:
            cell = engine::TrackerCell{};
            break;
        case 1:
            cell.instrument = 0;
            break;
        case 2:
            cell.volume = 0xFF;
            break;
        case 3:
            cell.effect = 0;
            cell.effect_param = 0;
            break;
        default:
            cell.effect_param = 0;
            break;
        }
        session.set_cell(pattern_index, cursor_.row, cursor_.channel, cell);
        advance_cursor_down(rows);
        return;
    }

    if (cursor_.field == 0) {
        // Track-bind shortcut: bound channels leave instrument = 0 and
        // resolve at playback (web TrackerCanvas convention).
        std::array<int, engine::kMaxSamples> bound{};
        const int bound_count = engine::track_bound_instruments(project, cursor_.channel,
                                                                bound.data(), engine::kMaxSamples);
        const int default_instrument = bound_count > 0 ? 0 : selected_slot_;

        // Space / = : note-off.
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Equal, false)) {
            engine::TrackerCell cell = current;
            cell.note = engine::kNoteOff;
            cell.instrument = static_cast<std::uint8_t>(default_instrument);
            session.set_cell(pattern_index, cursor_.row, cursor_.channel, cell);
            advance_cursor_down(rows);
            return;
        }

        for (const NoteKey& nk : kNoteKeys) {
            if (ImGui::IsKeyPressed(nk.key, false) && !io.KeyCtrl) {
                const int note = std::min(96, nk.offset + (octave_ * 12));
                // Shared with MIDI step entry (app/midi_record) and the
                // NOTE ENTRY panel's on-screen keyboard: bound-resolution,
                // cell write and audition live in enter_note_cell so every
                // entry path stays identical. default_volume_ is -1 by
                // default (leave the volume column), so a keystroke and a
                // panel key land the same cell.
                app::enter_note_cell(session, pattern_index, cursor_.row, cursor_.channel, note,
                                     selected_slot_, default_volume_, true);
                last_note_ = note;
                advance_cursor_down(rows);
                return;
            }
        }
        return;
    }

    // Hex entry: byte fields shift the low nibble up (web convention).
    for (const auto& [key, hex] : kHexKeys) {
        if (!ImGui::IsKeyPressed(key, false) || io.KeyCtrl) {
            continue;
        }
        engine::TrackerCell cell = current;
        switch (cursor_.field) {
        case 1: {
            const int cur = cell.instrument;
            const int next = ((cur & 0x0F) << 4) | hex;
            cell.instrument = static_cast<std::uint8_t>(std::min(31, next != 0 ? next : hex));
            break;
        }
        case 2: {
            const int cur = cell.volume == 0xFF ? 0 : cell.volume;
            cell.volume = static_cast<std::uint8_t>(std::min(0x40, ((cur & 0x0F) << 4) | hex));
            break;
        }
        case 3:
            cell.effect = static_cast<std::uint8_t>(hex);
            break;
        default:
            cell.effect_param =
                static_cast<std::uint8_t>((((cell.effect_param) & 0x0F) << 4) | hex);
            break;
        }
        session.set_cell(pattern_index, cursor_.row, cursor_.channel, cell);
        return;
    }
}

void PatternView::draw_transport(app::ProjectSession& session,
                                 const audio::EngineSnapshot& snapshot, const Theme& theme) const {
    if (ImGui::Button(snapshot.transport_playing ? "STOP (F5)" : "PLAY (F5)")) {
        if (snapshot.transport_playing) {
            session.stop();
        } else {
            session.play();
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(theme.text, "BPM %d  SPD %d", session.project().bpm,
                       session.project().speed);
    ImGui::SameLine();
    ImGui::TextColored(theme.text_dim, "OCT %d (F1/F2)", octave_);
    ImGui::SameLine();
    ImGui::TextColored(theme.text_dim, "SMP %02X", selected_slot_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!session.undo().can_undo());
    if (ImGui::SmallButton("UNDO")) {
        session.undo().undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!session.undo().can_redo());
    if (ImGui::SmallButton("REDO")) {
        session.undo().redo();
    }
    ImGui::EndDisabled();
    if (snapshot.transport_playing) {
        ImGui::SameLine();
        ImGui::TextColored(theme.primary, "ROW %02X", snapshot.row);
    }
}

void PatternView::draw_order_list(app::ProjectSession& session,
                                  const audio::EngineSnapshot& snapshot, const Theme& theme) {
    engine::TrackerProject& project = session.project();
    ImGui::TextColored(theme.primary, "ORDER");
    for (std::size_t i = 0; i < project.order_list.size(); ++i) {
        const int pat = project.order_list[i];
        const bool is_current =
            snapshot.transport_playing && static_cast<int>(i) == snapshot.order_pos;
        const bool is_edit = pat == edit_pattern_;
        std::array<char, 32> label{};
        std::snprintf(label.data(), label.size(), "%s%02zu PAT%02d", is_current ? ">" : " ", i,
                      pat);
        if (ImGui::Selectable(label.data(), is_edit)) {
            edit_pattern_ = pat;
            cursor_.row = 0;
            scroll_row_ = 0;
            selection_.active = false;
        }
    }
}

void PatternView::draw_grid(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                            const Theme& theme, int pattern_index) {
    engine::TrackerProject& project = session.project();
    const engine::TrackerPattern& pattern =
        project.patterns[static_cast<std::size_t>(pattern_index)];
    const int rows = static_cast<int>(pattern.rows.size());
    const int channels = project.channels;

    const GridMetrics m = metrics();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    const int visible_rows = std::max(1, static_cast<int>((avail.y - m.header_h) / m.row_h));

    // Follow the playhead while playing (web canvas behaviour): keep
    // the play row centred once it leaves the visible band.
    if (snapshot.transport_playing && snapshot.pattern_index == pattern_index) {
        if (snapshot.row < scroll_row_ || snapshot.row >= scroll_row_ + visible_rows) {
            scroll_row_ = std::max(0, snapshot.row - (visible_rows / 2));
        }
    }
    clamp_scroll(rows, visible_rows);

    const int visible_cols = std::max(1, static_cast<int>((avail.x - m.row_num_w) / m.col_w));
    channel_scroll_ = std::clamp(channel_scroll_, 0, std::max(0, channels - 1));
    if (cursor_.channel < channel_scroll_) {
        channel_scroll_ = cursor_.channel;
    }
    if (cursor_.channel >= channel_scroll_ + visible_cols) {
        channel_scroll_ = cursor_.channel - visible_cols + 1;
    }
    const int start_ch = channel_scroll_;
    const int end_ch = std::min(channels, start_ch + visible_cols);

    const int h_minor =
        pattern.highlight_minor > 0 ? pattern.highlight_minor : project.highlight_minor;

    // Channel headers: hue strip + CH label + bound pills.
    for (int ch = start_ch; ch < end_ch; ++ch) {
        const float cx = origin.x + m.row_num_w + (static_cast<float>(ch - start_ch) * m.col_w);
        const ImU32 hue =
            project.channel_colors[static_cast<std::size_t>(ch)] != 0
                ? (0xFF000000U | (project.channel_colors[static_cast<std::size_t>(ch)] & 0xFFFFFFU))
                : channel_color(ch);
        draw->AddRectFilled({cx + 1, origin.y + 1}, {cx + m.col_w - 1, origin.y + 3}, hue);
        std::array<char, 16> label{};
        std::snprintf(label.data(), label.size(), "CH%d", ch + 1);
        const float lw = ImGui::CalcTextSize(label.data()).x;
        draw->AddText({cx + ((m.col_w - lw) / 2), origin.y + 5}, col(theme.primary), label.data());

        std::array<int, engine::kMaxSamples> bound{};
        const int bound_count =
            engine::track_bound_instruments(project, ch, bound.data(), engine::kMaxSamples);
        if (bound_count > 0) {
            const float pill_w = m.char_w * 2.2F;
            const float pill_h = m.row_h * 0.6F;
            float px = cx + 4;
            const float py = origin.y + m.header_h - pill_h - 3;
            const int max_visible = std::min(3, bound_count);
            for (int bi = 0; bi < max_visible; ++bi) {
                std::array<char, 4> hex{};
                std::snprintf(hex.data(), hex.size(), "%02X", bound[static_cast<std::size_t>(bi)]);
                if (bi == 0) {
                    draw->AddRectFilled({px, py}, {px + pill_w, py + pill_h}, hue);
                    draw->AddText({px + 2, py}, col(theme.background), hex.data());
                } else {
                    draw->AddRect({px, py}, {px + pill_w, py + pill_h}, hue);
                    draw->AddText({px + 2, py}, hue, hex.data());
                }
                px += pill_w + 2;
            }
        }
    }
    draw->AddLine({origin.x, origin.y + m.header_h - 1},
                  {origin.x + avail.x, origin.y + m.header_h - 1}, col(theme.border));

    const bool playing_this_pattern =
        snapshot.transport_playing && snapshot.pattern_index == pattern_index;

    // Selection block normalized for drawing (mirror of the ops'
    // normalization, so the highlight covers exactly the cells an
    // operation would touch): rows and channels ordered, the field
    // edges following their channels.
    app::CellSelection sel = block_selection(pattern_index);
    if (sel.row0 > sel.row1) {
        std::swap(sel.row0, sel.row1);
    }
    if (sel.channel0 > sel.channel1) {
        std::swap(sel.channel0, sel.channel1);
        std::swap(sel.field0, sel.field1);
    } else if (sel.channel0 == sel.channel1 && sel.field0 > sel.field1) {
        std::swap(sel.field0, sel.field1);
    }

    const int start_row = scroll_row_;
    const int end_row = std::min(rows, start_row + visible_rows);
    for (int row = start_row; row < end_row; ++row) {
        const float ry = origin.y + m.header_h + (static_cast<float>(row - start_row) * m.row_h);
        const bool is_play_row = playing_this_pattern && row == snapshot.row;
        const bool is_beat_row = h_minor > 0 && row % h_minor == 0;

        if (is_play_row) {
            draw->AddRectFilled({origin.x, ry}, {origin.x + avail.x, ry + m.row_h},
                                col(theme.primary, 0.25F));
        } else if (is_beat_row) {
            draw->AddRectFilled({origin.x, ry}, {origin.x + avail.x, ry + m.row_h},
                                col(theme.background_alt));
        }

        std::array<char, 4> row_num{};
        std::snprintf(row_num.data(), row_num.size(), "%02X", row);
        draw->AddText({origin.x + 4, ry + 1}, col(is_beat_row ? theme.primary : theme.text_dim),
                      row_num.data());

        for (int ch = start_ch; ch < end_ch; ++ch) {
            const engine::TrackerCell& cell =
                pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(ch)];
            const float cx = origin.x + m.row_num_w + (static_cast<float>(ch - start_ch) * m.col_w);

            // Selection highlight under the text; edge channels trim
            // to the selected sub-columns. The cursor rect paints over
            // it so the cursor stays visible inside a block.
            if (selection_.active && row >= sel.row0 && row <= sel.row1 && ch >= sel.channel0 &&
                ch <= sel.channel1) {
                const int f0 = ch == sel.channel0 ? static_cast<int>(sel.field0) : 0;
                const int f1 =
                    ch == sel.channel1 ? static_cast<int>(sel.field1) : app::kCellFieldCount - 1;
                const float x0 = cx + (kFieldLeft[static_cast<std::size_t>(f0)] * m.char_w);
                const float x1 = std::min(
                    cx + (kFieldRight[static_cast<std::size_t>(f1)] * m.char_w), cx + m.col_w - 2);
                // Selection fill from the highlight colour (dark themes:
                // highlight_bg == primary, so identical to the old glow;
                // arctic's 0.08 glow would leave the block invisible).
                draw->AddRectFilled({x0, ry}, {x1, ry + m.row_h}, col(theme.highlight_bg, 0.35F));
            }

            const bool is_cursor_cell = row == cursor_.row && ch == cursor_.channel;
            if (is_cursor_cell) {
                draw->AddRectFilled({cx, ry}, {cx + m.col_w - 2, ry + m.row_h}, col(theme.border));
            }

            // Field text, coloured per the web drawCell conventions.
            std::array<char, 4> note_text{};
            engine::note_name(cell.note, note_text.data());
            const int resolved =
                (cell.instrument == 0 && cell.note > 0 && cell.note <= engine::kMaxNote)
                    ? engine::resolve_bound_instrument(project, ch, cell.bound_index)
                    : 0;
            const int slot = cell.instrument > 0 ? cell.instrument : resolved;

            std::array<char, 12> ins{};
            if (slot > 0) {
                std::snprintf(ins.data(), ins.size(), "%02X", slot);
            } else {
                std::snprintf(ins.data(), ins.size(), "--");
            }
            std::array<char, 4> vol{};
            if (cell.volume != 0xFF) {
                std::snprintf(vol.data(), vol.size(), "%02X", cell.volume);
            } else {
                std::snprintf(vol.data(), vol.size(), "--");
            }
            std::array<char, 4> eff = {'.', '\0'};
            if (cell.effect != 0) {
                std::snprintf(eff.data(), eff.size(), "%X", cell.effect);
            }
            std::array<char, 4> prm{};
            if (cell.effect_param != 0) {
                std::snprintf(prm.data(), prm.size(), "%02X", cell.effect_param);
            } else {
                std::snprintf(prm.data(), prm.size(), "..");
            }

            ImU32 ins_color = col(theme.text_dim);
            if (cell.instrument > 0) {
                ins_color = col(theme.text);
            } else if (resolved > 0) {
                ins_color = col(theme.primary_dim);
            }
            float tx = cx + 3;
            const float ty = ry + 1;
            const auto put = [&](const char* text, ImU32 color) {
                draw->AddText({tx, ty}, color, text);
                tx += ImGui::CalcTextSize(text).x;
            };
            put(note_text.data(), cell.note > 0 ? col(theme.primary) : col(theme.text_dim));
            put(" ", col(theme.text_dim));
            put(ins.data(), ins_color);
            put(" ", col(theme.text_dim));
            put(vol.data(), cell.volume != 0xFF ? col(theme.text) : col(theme.text_dim));
            put(" ", col(theme.text_dim));
            put(eff.data(), cell.effect != 0 ? col(theme.primary_dim) : col(theme.text_dim));
            put(prm.data(), cell.effect_param != 0 ? col(theme.text) : col(theme.text_dim));
        }
    }

    // Column separators.
    for (int ch = start_ch + 1; ch < end_ch; ++ch) {
        const float cx = origin.x + m.row_num_w + (static_cast<float>(ch - start_ch) * m.col_w) - 1;
        draw->AddLine({cx, origin.y + m.header_h - 2}, {cx, origin.y + avail.y}, col(theme.border));
    }

    // Grid mouse: press moves the cursor and re-anchors, dragging away
    // from the pressed cell opens a selection, right-click offers the
    // block operations (field resolved from x within the cell).
    ImGui::InvisibleButton("grid", avail);
    const auto cell_under = [&](const ImVec2& mouse, Cursor& out) {
        const float local_x = mouse.x - origin.x - m.row_num_w;
        const float local_y = mouse.y - origin.y - m.header_h;
        if (local_x < 0 || local_y < 0) {
            return false;
        }
        const int ch = start_ch + static_cast<int>(local_x / m.col_w);
        const int row = start_row + static_cast<int>(local_y / m.row_h);
        if (ch >= channels || row >= rows) {
            return false;
        }
        out.channel = ch;
        out.row = row;
        const float in_cell = local_x - (static_cast<float>(ch - start_ch) * m.col_w);
        const float u = in_cell / m.char_w;
        // Field bands in character units: note(4) inst(3) vol(3)
        // effect(1.5) param(rest).
        if (u < 4) {
            out.field = 0;
        } else if (u < 7) {
            out.field = 1;
        } else if (u < 10) {
            out.field = 2;
        } else if (u < 11.5F) {
            out.field = 3;
        } else {
            out.field = 4;
        }
        return true;
    };
    Cursor hit;
    if (ImGui::IsItemActivated() && cell_under(ImGui::GetMousePos(), hit)) {
        cursor_ = hit;
        selection_ = {hit.row, hit.channel, hit.field, false};
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0F) &&
        cell_under(ImGui::GetMousePos(), hit)) {
        cursor_ = hit;
        selection_.active = hit.row != selection_.row || hit.channel != selection_.channel ||
                            hit.field != selection_.field;
    }
    draw_context_menu(session, pattern_index);
}

void PatternView::draw_context_menu(app::ProjectSession& session, int pattern_index) {
    if (!ImGui::BeginPopupContextItem("block_ops")) {
        return;
    }
    const app::CellSelection block = block_selection(pattern_index);
    if (ImGui::MenuItem("COPY", "CTRL+C")) {
        clipboard_ = app::copy_cells(session, block);
    }
    if (ImGui::MenuItem("CUT", "CTRL+X")) {
        clipboard_ = app::cut_cells(session, block);
    }
    if (ImGui::MenuItem("PASTE", "CTRL+V", false, !clipboard_.empty())) {
        app::paste_cells(session, clipboard_, pattern_index, cursor_.row, cursor_.channel);
    }
    if (ImGui::MenuItem("CLEAR", "DEL")) {
        app::clear_cells(session, block);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("TRANSPOSE -1", "ALT+F1")) {
        app::transpose_cells(session, block, -1);
    }
    if (ImGui::MenuItem("TRANSPOSE +1", "ALT+F2")) {
        app::transpose_cells(session, block, 1);
    }
    if (ImGui::MenuItem("OCTAVE -1", "ALT+F3")) {
        app::transpose_cells(session, block, -12);
    }
    if (ImGui::MenuItem("OCTAVE +1", "ALT+F4")) {
        app::transpose_cells(session, block, 12);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("INTERPOLATE", "CTRL+I")) {
        app::interpolate_cells(session, block);
    }
    ImGui::EndPopup();
}

void PatternView::draw(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                       const Theme& theme) {
    engine::TrackerProject& project = session.project();
    edit_pattern_ = std::clamp(edit_pattern_, 0, static_cast<int>(project.patterns.size()) - 1);

    // While playing, the edit view follows the playhead pattern (web
    // behaviour: the canvas always renders playState.patternIndex).
    const int pattern_index = snapshot.transport_playing ? snapshot.pattern_index : edit_pattern_;
    active_pattern_ = pattern_index;
    active_rows_ =
        static_cast<int>(project.patterns[static_cast<std::size_t>(pattern_index)].rows.size());

    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("PATTERN")) {
        draw_transport(session, snapshot, theme);
        ImGui::Separator();

        const GridMetrics m = metrics();
        if (ImGui::BeginChild("order", ImVec2(m.char_w * 14.0F, 0), ImGuiChildFlags_Borders)) {
            draw_order_list(session, snapshot, theme);
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("grid", ImVec2(0, 0), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar)) {
            const auto& pattern = project.patterns[static_cast<std::size_t>(pattern_index)];
            cursor_.channel = std::clamp(cursor_.channel, 0, project.channels - 1);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                handle_keys(session, pattern, pattern_index, visible_rows_);
            }
            draw_grid(session, snapshot, theme, pattern_index);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace nt::ui
