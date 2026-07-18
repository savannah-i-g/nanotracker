// ui/pattern_view — the pattern editor window: order list, transport
// strip, and the cell grid with tracker keyboard editing.
// Visual conventions and the interaction model port the web app's
// trackerRenderer.ts drawPatternEditor/drawOrderList and
// TrackerCanvas.tsx key handling (arrows/Tab field navigation,
// two-nibble hex entry, note keys with octave, edit-step advance,
// F5/F8 transport, Delete clears by field, space/= note-off).
#pragma once

#include "app/project_session.h"
#include "audio/audio_engine.h"
#include "ui/theme.h"

namespace nt::ui {

class PatternView {
public:
    // Draws the window and processes editing input while focused.
    void draw(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
              const Theme& theme);

private:
    struct Cursor {
        int row = 0;
        int channel = 0;
        int field = 0; // 0=note 1=inst 2=vol 3=effect 4=param
    };

    void handle_keys(app::ProjectSession& session, const engine::TrackerPattern& pattern,
                     int pattern_index, int visible_rows);
    void draw_transport(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                        const Theme& theme) const;
    void draw_order_list(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                         const Theme& theme);
    void draw_grid(app::ProjectSession& session, const audio::EngineSnapshot& snapshot,
                   const Theme& theme, int pattern_index);
    void advance_cursor_down(int rows);
    void clamp_scroll(int rows, int visible_rows);

    Cursor cursor_;
    int scroll_row_ = 0;
    int channel_scroll_ = 0;
    int octave_ = 4;
    int selected_slot_ = 1;
    int edit_pattern_ = 0; // pattern being edited (order-list selection)
    int visible_rows_ = 20;
};

} // namespace nt::ui
