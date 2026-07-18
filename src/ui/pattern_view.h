// ui/pattern_view — the pattern editor window: order list, transport
// strip, and the cell grid with tracker keyboard editing.
// Visual conventions and the interaction model port the web app's
// trackerRenderer.ts drawPatternEditor/drawOrderList and
// TrackerCanvas.tsx key handling (arrows/Tab field navigation,
// two-nibble hex entry, note keys with octave, edit-step advance,
// F5/F8 transport, Delete clears by field, space/= note-off).
#pragma once

#include "app/midi_record.h"
#include "app/pattern_ops.h"
#include "app/project_session.h"
#include "audio/audio_engine.h"
#include "ui/theme.h"

namespace nt::ui {

class PatternView : public app::PatternCursor {
public:
    // Draws the window and processes editing input while focused.
    void draw(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
              const Theme& theme);

    // app::PatternCursor — the MIDI step-entry surface. Coordinates
    // mirror the last drawn frame (active_pattern_/active_rows_ are
    // cached in draw()); step_advance is the keyboard edit-step
    // advance, so MIDI step entry lands and moves exactly like typing.
    [[nodiscard]] int step_pattern() const override { return active_pattern_; }

    [[nodiscard]] int step_row() const override { return cursor_.row; }

    [[nodiscard]] int step_channel() const override { return cursor_.channel; }

    [[nodiscard]] int step_entry_slot() const override { return selected_slot_; }

    void step_advance() override { advance_cursor_down(active_rows_); }

private:
    struct Cursor {
        int row = 0;
        int channel = 0;
        int field = 0; // 0=note 1=inst 2=vol 3=effect 4=param
    };

    // Block-selection anchor in cursor coordinates; the cursor itself
    // is the moving corner. Inactive means cursor-only editing, and
    // every block operation then acts on the cursor cell as a 1×1
    // selection.
    struct Anchor {
        int row = 0;
        int channel = 0;
        int field = 0; // cursor field units, see Cursor
        bool active = false;
    };

    void handle_keys(app::ProjectSession& session, const engine::TrackerPattern& pattern,
                     int pattern_index, int visible_rows);
    // The active selection as a pattern_ops block (unnormalized — the
    // ops normalize), or the cursor cell when no selection is active.
    [[nodiscard]] app::CellSelection block_selection(int pattern_index) const;
    void draw_context_menu(app::ProjectSession& session, int pattern_index);
    void draw_transport(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                        const Theme& theme) const;
    void draw_order_list(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                         const Theme& theme);
    void draw_grid(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                   const Theme& theme, int pattern_index);
    void advance_cursor_down(int rows);
    void clamp_scroll(int rows, int visible_rows);

    Cursor cursor_;
    Anchor selection_;
    app::CellClipboard clipboard_;
    int scroll_row_ = 0;
    int channel_scroll_ = 0;
    int octave_ = 4;
    int selected_slot_ = 1;
    int edit_pattern_ = 0; // pattern being edited (order-list selection)
    int visible_rows_ = 20;
    // Pattern the cursor writes to this frame (edit_pattern_, or the
    // playhead pattern while playing) and its row count — cached by
    // draw() for the PatternCursor surface.
    int active_pattern_ = 0;
    int active_rows_ = 64;
};

} // namespace nt::ui
