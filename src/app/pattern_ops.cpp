#include "app/pattern_ops.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace nt::app {

namespace {

constexpr std::uint8_t kVolumeEmpty = 0xFF;

std::uint8_t field_bit(CellField field) {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(field));
}

bool cells_equal(const engine::TrackerCell& a, const engine::TrackerCell& b) {
    return a.note == b.note && a.instrument == b.instrument && a.volume == b.volume &&
           a.effect == b.effect && a.effect_param == b.effect_param &&
           a.bound_index == b.bound_index;
}

// Selection normalized (row0 <= row1, channel0 <= channel1 with the
// field edges following their channels) and clamped to the pattern.
// nullopt when the pattern index is invalid or nothing overlaps.
std::optional<CellSelection> normalized(const engine::TrackerProject& project, CellSelection sel) {
    if (sel.pattern < 0 || sel.pattern >= static_cast<int>(project.patterns.size())) {
        return std::nullopt;
    }
    const auto& pattern = project.patterns[static_cast<std::size_t>(sel.pattern)];
    const int rows = static_cast<int>(pattern.rows.size());
    if (rows == 0 || project.channels <= 0) {
        return std::nullopt;
    }
    if (sel.row0 > sel.row1) {
        std::swap(sel.row0, sel.row1);
    }
    if (sel.channel0 > sel.channel1) {
        std::swap(sel.channel0, sel.channel1);
        std::swap(sel.field0, sel.field1);
    } else if (sel.channel0 == sel.channel1 && sel.field0 > sel.field1) {
        std::swap(sel.field0, sel.field1);
    }
    if (sel.row1 < 0 || sel.row0 >= rows || sel.channel1 < 0 || sel.channel0 >= project.channels) {
        return std::nullopt;
    }
    // Clamping an edge into range widens that channel to full fields.
    if (sel.channel0 < 0) {
        sel.channel0 = 0;
        sel.field0 = CellField::kNote;
    }
    if (sel.channel1 >= project.channels) {
        sel.channel1 = project.channels - 1;
        sel.field1 = CellField::kEffect;
    }
    sel.row0 = std::clamp(sel.row0, 0, rows - 1);
    sel.row1 = std::clamp(sel.row1, 0, rows - 1);
    return sel;
}

// Fields the (normalized) selection covers on one channel: interior
// channels are full, the edge channels trim to field0/field1.
std::uint8_t channel_field_mask(const CellSelection& sel, int channel) {
    const int first = channel == sel.channel0 ? static_cast<int>(sel.field0) : 0;
    const int last = channel == sel.channel1 ? static_cast<int>(sel.field1) : kCellFieldCount - 1;
    std::uint8_t mask = 0;
    for (int f = first; f <= last; ++f) {
        mask |= field_bit(static_cast<CellField>(f));
    }
    return mask;
}

// Overwrites the masked fields of `dst` from `src`.
void merge_fields(engine::TrackerCell& dst, const engine::TrackerCell& src, std::uint8_t mask) {
    if ((mask & field_bit(CellField::kNote)) != 0) {
        dst.note = src.note;
    }
    if ((mask & field_bit(CellField::kInstrument)) != 0) {
        dst.instrument = src.instrument;
        dst.bound_index = src.bound_index;
    }
    if ((mask & field_bit(CellField::kVolume)) != 0) {
        dst.volume = src.volume;
    }
    if ((mask & field_bit(CellField::kEffect)) != 0) {
        dst.effect = src.effect;
        dst.effect_param = src.effect_param;
    }
}

void clear_fields(engine::TrackerCell& cell, std::uint8_t mask) {
    merge_fields(cell, engine::TrackerCell{}, mask);
}

const engine::TrackerCell& cell_at(const engine::TrackerProject& project, int pattern, int row,
                                   int channel) {
    return project.patterns[static_cast<std::size_t>(pattern)]
        .rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)];
}

// Clears the selection's fields inside an already-open undo group.
void clear_cells_in_group(ProjectSession& session, const CellSelection& sel) {
    for (int row = sel.row0; row <= sel.row1; ++row) {
        for (int channel = sel.channel0; channel <= sel.channel1; ++channel) {
            const engine::TrackerCell& current =
                cell_at(session.project(), sel.pattern, row, channel);
            engine::TrackerCell cleared = current;
            clear_fields(cleared, channel_field_mask(sel, channel));
            if (!cells_equal(cleared, current)) {
                session.set_cell(sel.pattern, row, channel, cleared);
            }
        }
    }
}

} // namespace

CellClipboard copy_cells(const ProjectSession& session, const CellSelection& selection) {
    CellClipboard clipboard;
    const auto sel = normalized(session.project(), selection);
    if (!sel.has_value()) {
        return clipboard;
    }
    clipboard.rows = sel->row1 - sel->row0 + 1;
    clipboard.channels = sel->channel1 - sel->channel0 + 1;
    clipboard.cells.reserve(static_cast<std::size_t>(clipboard.rows) *
                            static_cast<std::size_t>(clipboard.channels));
    for (int channel = sel->channel0; channel <= sel->channel1; ++channel) {
        clipboard.field_masks.push_back(channel_field_mask(*sel, channel));
    }
    for (int row = sel->row0; row <= sel->row1; ++row) {
        for (int channel = sel->channel0; channel <= sel->channel1; ++channel) {
            // Captured cells keep only their masked fields so pasting
            // an uncaptured field is a guaranteed no-op.
            engine::TrackerCell cell;
            merge_fields(cell, cell_at(session.project(), sel->pattern, row, channel),
                         clipboard.field_masks[static_cast<std::size_t>(channel - sel->channel0)]);
            clipboard.cells.push_back(cell);
        }
    }
    return clipboard;
}

CellClipboard cut_cells(ProjectSession& session, const CellSelection& selection) {
    CellClipboard clipboard = copy_cells(session, selection);
    if (clipboard.empty()) {
        return clipboard;
    }
    // A non-empty clipboard implies the selection normalizes.
    const auto sel = normalized(session.project(), selection);
    if (sel.has_value()) {
        const UndoGroup group(session.undo(), "cut cells");
        clear_cells_in_group(session, *sel);
    }
    return clipboard;
}

void paste_cells(ProjectSession& session, const CellClipboard& clipboard, int pattern, int row,
                 int channel) {
    if (clipboard.empty()) {
        return;
    }
    const engine::TrackerProject& project = session.project();
    if (pattern < 0 || pattern >= static_cast<int>(project.patterns.size())) {
        return;
    }
    const int rows =
        static_cast<int>(project.patterns[static_cast<std::size_t>(pattern)].rows.size());
    const UndoGroup group(session.undo(), "paste cells");
    for (int r = 0; r < clipboard.rows; ++r) {
        const int dst_row = row + r;
        if (dst_row < 0 || dst_row >= rows) {
            continue;
        }
        for (int c = 0; c < clipboard.channels; ++c) {
            const int dst_channel = channel + c;
            if (dst_channel < 0 || dst_channel >= project.channels) {
                continue;
            }
            const engine::TrackerCell& current = cell_at(project, pattern, dst_row, dst_channel);
            engine::TrackerCell merged = current;
            merge_fields(merged,
                         clipboard.cells[static_cast<std::size_t>(r) *
                                             static_cast<std::size_t>(clipboard.channels) +
                                         static_cast<std::size_t>(c)],
                         clipboard.field_masks[static_cast<std::size_t>(c)]);
            if (!cells_equal(merged, current)) {
                session.set_cell(pattern, dst_row, dst_channel, merged);
            }
        }
    }
}

void clear_cells(ProjectSession& session, const CellSelection& selection) {
    const auto sel = normalized(session.project(), selection);
    if (!sel.has_value()) {
        return;
    }
    const UndoGroup group(session.undo(), "clear cells");
    clear_cells_in_group(session, *sel);
}

void transpose_cells(ProjectSession& session, const CellSelection& selection, int semitones) {
    const auto sel = normalized(session.project(), selection);
    if (!sel.has_value() || semitones == 0) {
        return;
    }
    const UndoGroup group(session.undo(), "transpose cells");
    for (int channel = sel->channel0; channel <= sel->channel1; ++channel) {
        if ((channel_field_mask(*sel, channel) & field_bit(CellField::kNote)) == 0) {
            continue;
        }
        for (int row = sel->row0; row <= sel->row1; ++row) {
            const engine::TrackerCell& current =
                cell_at(session.project(), sel->pattern, row, channel);
            // Only real notes move; empty (0) and note-off stay.
            if (current.note < 1 || current.note > engine::kMaxNote) {
                continue;
            }
            const int shifted = std::clamp(current.note + semitones, 1, engine::kMaxNote);
            if (shifted != current.note) {
                engine::TrackerCell cell = current;
                cell.note = static_cast<std::uint8_t>(shifted);
                session.set_cell(sel->pattern, row, channel, cell);
            }
        }
    }
}

bool interpolate_cells(ProjectSession& session, const CellSelection& selection) {
    const auto sel = normalized(session.project(), selection);
    if (!sel.has_value() || sel->row1 - sel->row0 < 2) {
        return false;
    }
    const int span = sel->row1 - sel->row0;
    bool filled = false;
    const UndoGroup group(session.undo(), "interpolate cells");
    for (int channel = sel->channel0; channel <= sel->channel1; ++channel) {
        const std::uint8_t mask = channel_field_mask(*sel, channel);
        const engine::TrackerCell first =
            cell_at(session.project(), sel->pattern, sel->row0, channel);
        const engine::TrackerCell last =
            cell_at(session.project(), sel->pattern, sel->row1, channel);

        const bool fill_volume = (mask & field_bit(CellField::kVolume)) != 0 &&
                                 first.volume != kVolumeEmpty && last.volume != kVolumeEmpty;
        const bool fill_effect = (mask & field_bit(CellField::kEffect)) != 0 &&
                                 first.effect == last.effect &&
                                 (first.effect != 0 || first.effect_param != 0) &&
                                 (last.effect != 0 || last.effect_param != 0);
        if (!fill_volume && !fill_effect) {
            continue;
        }
        filled = true;
        for (int row = sel->row0 + 1; row < sel->row1; ++row) {
            const double t = static_cast<double>(row - sel->row0) / span;
            const engine::TrackerCell& current =
                cell_at(session.project(), sel->pattern, row, channel);
            engine::TrackerCell cell = current;
            if (fill_volume) {
                cell.volume = static_cast<std::uint8_t>(
                    std::lround(first.volume + (last.volume - first.volume) * t));
            }
            if (fill_effect) {
                cell.effect = first.effect;
                cell.effect_param = static_cast<std::uint8_t>(
                    std::lround(first.effect_param + (last.effect_param - first.effect_param) * t));
            }
            if (!cells_equal(cell, current)) {
                session.set_cell(sel->pattern, row, channel, cell);
            }
        }
    }
    return filled;
}

} // namespace nt::app
