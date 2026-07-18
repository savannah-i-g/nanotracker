// ui/history_view — the HISTORY window: the linear edit list over
// app::UndoStack. Done edits sit above the current position and the redo
// branch below; clicking a row jumps there by driving the session's
// undo/redo path (each jump is one UI-thread batch this frame). Sample-
// op entries — the bounded buffer-snapshot edits — carry a tag.
//
// Structural edits reshape the project outside the undo model and wipe
// the stack; when that just happened the panel says so honestly (the
// UndoStack clear breadcrumb) rather than showing a stale or bare-empty
// list. The panel holds no history state of its own — it reads through
// UndoStack's const accessors and mutates only through session undo/redo.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <cstddef>

namespace nt::ui {

class HistoryView {
public:
    // `open` is the VIEW-menu visibility flag; the window's close box
    // clears it, matching the DEBUG window's contract.
    void draw(app::ProjectSession& session, const Theme& theme, bool& open);

private:
    // Last-seen split, so the list can auto-scroll the current position
    // back into view whenever it moves (a jump, Ctrl+Z/Y, or a new edit).
    std::size_t last_undo_depth_ = 0;
    std::size_t last_redo_depth_ = 0;
    bool primed_ = false;
};

} // namespace nt::ui
