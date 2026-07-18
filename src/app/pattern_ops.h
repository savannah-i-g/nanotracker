// app/pattern_ops — block editing over the pattern grid.
// Free functions that read the project through ProjectSession and
// write exclusively through ProjectSession::set_cell inside an undo
// group, so each block operation lands as a single undo entry (and a
// no-op operation lands as none). No UI state lives here: the pattern
// view owns selection gestures and calls in with a CellSelection.
#pragma once

#include "app/project_session.h"
#include "engine/tracker_types.h"

#include <cstdint>
#include <vector>

namespace nt::app {

// Sub-columns of a channel in editor layout order. The effect command
// and its parameter travel as one column (a parameter without its
// command is meaningless); the instrument column carries the
// bound-instrument sub-slot with it for the same reason.
enum class CellField : std::uint8_t {
    kNote = 0,
    kInstrument = 1,
    kVolume = 2,
    kEffect = 3, // effect command + parameter
};

inline constexpr int kCellFieldCount = 4;

// Inclusive block over one pattern's grid, column-aware in the
// classic-tracker sense: the block spans linearized sub-columns from
// (channel0, field0) to (channel1, field1) — interior channels are
// covered in full, `field0` trims the left edge and `field1` the
// right. Coordinates may arrive in any order; operations normalize
// and clamp to the pattern before use.
struct CellSelection {
    int pattern = 0;
    int row0 = 0;
    int row1 = 0;
    int channel0 = 0;
    int channel1 = 0;
    CellField field0 = CellField::kNote;
    CellField field1 = CellField::kEffect;
};

// Internal clipboard: a project-native cell block plus a per-channel
// field mask recording which sub-columns were captured. Paste merges
// only captured fields into the destination, so a volume-column copy
// never disturbs notes.
struct CellClipboard {
    int rows = 0;
    int channels = 0;
    std::vector<engine::TrackerCell> cells; // row-major, rows * channels
    std::vector<std::uint8_t> field_masks;  // per channel, bits of (1 << CellField)

    [[nodiscard]] bool empty() const { return rows <= 0 || channels <= 0; }
};

// Copies the selected block; the project is untouched. Returns an
// empty clipboard when the selection misses the pattern entirely.
CellClipboard copy_cells(const ProjectSession& session, const CellSelection& selection);

// Copy followed by clear, as one undo entry.
CellClipboard cut_cells(ProjectSession& session, const CellSelection& selection);

// Pastes the block with its top-left at (row, channel) of `pattern`,
// merging only captured fields and clipping at the pattern edges.
void paste_cells(ProjectSession& session, const CellClipboard& clipboard, int pattern, int row,
                 int channel);

// Resets the selected fields to their empty values (note/instrument/
// effect zero, volume 0xFF).
void clear_cells(ProjectSession& session, const CellSelection& selection);

// Shifts real notes by `semitones`, clamping into [1, kMaxNote];
// empty cells and note-off are untouched. Applies only where the
// selection covers the note column.
void transpose_cells(ProjectSession& session, const CellSelection& selection, int semitones);

// Linear fill across the selection's rows, per covered channel: the
// volume column when both endpoint rows have a set volume, and the
// effect parameter when both endpoint rows carry the same non-empty
// effect command (the command is copied onto the filled rows).
// Endpoint rows keep their values. Returns true when any column was
// filled.
bool interpolate_cells(ProjectSession& session, const CellSelection& selection);

} // namespace nt::app
