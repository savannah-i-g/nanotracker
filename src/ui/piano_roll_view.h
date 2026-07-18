// ui/piano_roll_view — sequence-layer editor. Behavioural reference:
// SequencePianoRollEditor.tsx; the model is the engine's sequence
// mixer (up to 4 layers per channel, notes in ticks — tracker rows are
// `speed` ticks wide). Click adds a note, right-click removes; a drag
// from empty space rubber-bands a selection, a selected note's body
// drags the whole selection, its right edge resizes that note alone.
// Ctrl+C/Ctrl+V run a view-local clipboard, and the tools strip
// applies engine/sequence_ops transforms to the selection (all notes
// when nothing is selected, matching the web toolbox).
//
// Single-note edits route through the session's per-note API (one
// structural republish each); batched edits — selection move, paste,
// tools — go through replace_notes, which swaps the layer's note
// vector under one transport stop and republishes once.
//
// The on-screen keyboard lives at the bottom of the window: two
// octaves of buttons that audition the selected layer's instrument
// through the preview path (sample or plugin alike).
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace nt::ui {

class PianoRollView {
public:
    void draw(app::ProjectSession& session, const Theme& theme);

private:
    // Grid mouse gesture; decided at press, resolved at release. A
    // stationary marquee doubles as the click gesture (clear / add).
    enum class Drag : std::uint8_t { kNone, kMarquee, kMove, kResize };

    // A note tagged with its post-edit selection membership;
    // replace_notes re-sorts by start tick and rebuilds selection_
    // from the tags, so callers never chase indices across the sort.
    struct TaggedNote {
        engine::SequenceNote note;
        bool selected = false;
    };

    using Transform =
        std::function<std::vector<engine::SequenceNote>(const std::vector<engine::SequenceNote>&)>;

    [[nodiscard]] bool is_selected(int note_index) const;
    void toggle_selected(int note_index);

    // Replaces the layer's notes wholesale: one transport stop, one
    // bundle republish for the whole batch (see the definition for the
    // contract with the session's per-note API).
    void replace_notes(app::ProjectSession& session, int pattern_index,
                       std::vector<TaggedNote> tagged);

    // Runs a sequence_ops transform over the selection (all notes when
    // none is active) and republishes once; the selection follows the
    // transformed notes.
    void apply_transform(app::ProjectSession& session, int pattern_index,
                         const engine::SequenceLayer& layer, const Transform& transform);

    // The tools strip above the grid; disabled while the layer has no
    // notes.
    void draw_toolbox(app::ProjectSession& session, int pattern_index,
                      const engine::SequenceLayer* layer, int ticks_per_row);

    int channel_ = 0;
    int layer_ = 0;
    int base_pitch_ = 56; // bottom row of the grid
    int keyboard_note_down_ = -1;

    std::vector<int> selection_;                  // note indices, ascending
    std::vector<engine::SequenceNote> clipboard_; // start ticks rebased to zero
    Drag drag_ = Drag::kNone;
    ImVec2 drag_press_; // grid-content px at mouse down
    int resize_note_ = -1;

    // Tool parameters shared by the apply buttons; the enum-backed
    // values follow engine/sequence_ops declaration order.
    int arp_direction_ = 0;
    int arp_rate_ = 4;
    int arp_octaves_ = 1;
    int velocity_shape_ = 0;

    // Humanize/arpeggiate jitter. Seeded once per view from hardware
    // entropy — determinism lives in the sequence_ops unit tests, the
    // editor wants fresh draws.
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace nt::ui
