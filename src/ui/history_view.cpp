#include "ui/history_view.h"

#include "app/undo.h"

#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

void HistoryView::draw(app::ProjectSession& session, const Theme& theme, bool& open) {
    // Floating panel (VIEW toggle, like DEBUG): opens top-right on first
    // use, out of the pattern grid's way; the user can dock it anywhere.
    // `open` is the VIEW-menu flag, so the window's close box mirrors it.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2{viewport->WorkPos.x + viewport->WorkSize.x - 320.0F, viewport->WorkPos.y + 40.0F},
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{300, 420}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("HISTORY", &open)) {
        ImGui::End();
        return;
    }

    app::UndoStack& undo = session.undo();
    const std::size_t undo_depth = undo.undo_depth();
    const std::size_t redo_depth = undo.redo_depth();
    const std::size_t sample_ops = undo.sample_op_depth();

    // ── Depth indicators ─────────────────────────────────────────────
    ImGui::TextColored(theme.text_dim, "%zu / %zu edits", undo_depth, app::UndoStack::kMaxEntries);
    if (sample_ops > 0) {
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim, "   sample ops %zu / %zu", sample_ops,
                           app::UndoStack::kMaxSampleOps);
    }
    ImGui::Separator();

    // Scroll the current position back into view whenever the split moved
    // since the last frame (a jump here, a grid Ctrl+Z/Y, or a new edit),
    // and on the first draw.
    const bool position_moved =
        !primed_ || undo_depth != last_undo_depth_ || redo_depth != last_redo_depth_;
    primed_ = true;
    last_undo_depth_ = undo_depth;
    last_redo_depth_ = redo_depth;

    // ── Empty / just-cleared state ───────────────────────────────────
    if (undo_depth == 0 && redo_depth == 0) {
        if (undo.was_cleared()) {
            ImGui::TextColored(theme.primary_dim, "history cleared: %s", undo.clear_reason());
            ImGui::TextWrapped("Structural edits (patterns, sequence notes, samples, workspace) "
                               "reshape the project outside the undo model, so they clear the "
                               "history. Extending their retention is recorded as follow-up.");
        } else {
            ImGui::TextColored(theme.text_dim, "no history yet");
            ImGui::TextWrapped(
                "Edits you can undo — cell writes, sample metadata, cables — appear here.");
        }
        ImGui::End();
        return;
    }

    // Jump requests are collected while enumerating and applied after the
    // walk, so the stacks are never mutated mid-list.
    std::size_t undo_steps = 0;
    std::size_t redo_steps = 0;

    ImGui::BeginChild("entries", ImVec2{0, 0});

    // Pre-history anchor: the project's starting point. Current when
    // everything is undone; otherwise clicking it undoes all the way.
    {
        const bool current = undo_depth == 0;
        if (ImGui::Selectable("(initial state)", current) && !current) {
            undo_steps = undo_depth;
        }
        if (current && position_moved) {
            ImGui::SetScrollHereY(0.5F);
        }
    }

    // Done edits, oldest → newest; the newest is the current position.
    for (std::size_t i = 0; i < undo_depth; ++i) {
        const app::UndoStack::HistoryEntry entry = undo.undo_at(i);
        const bool current = (i + 1 == undo_depth);
        std::array<char, 160> label{};
        std::snprintf(label.data(), label.size(), "%s%s##u%zu", entry.label,
                      entry.sample_op ? "   [buf]" : "", i);
        if (ImGui::Selectable(label.data(), current) && !current) {
            undo_steps = undo_depth - 1 - i;
        }
        if (current && position_moved) {
            ImGui::SetScrollHereY(0.5F);
        }
    }

    // Redo branch (future edits), next → last, dimmed. Clicking replays
    // up to and including that entry.
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text_dim);
    for (std::size_t j = 0; j < redo_depth; ++j) {
        const app::UndoStack::HistoryEntry entry = undo.redo_at(j);
        std::array<char, 160> label{};
        std::snprintf(label.data(), label.size(), "%s%s##r%zu", entry.label,
                      entry.sample_op ? "   [buf]" : "", j);
        if (ImGui::Selectable(label.data())) {
            redo_steps = j + 1;
        }
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();

    // Apply the jump as one batch of session-routed steps, each guarded so
    // a miscount can never spin past an empty stack.
    for (std::size_t s = 0; s < undo_steps && undo.can_undo(); ++s) {
        undo.undo();
    }
    for (std::size_t s = 0; s < redo_steps && undo.can_redo(); ++s) {
        undo.redo();
    }

    ImGui::End();
}

} // namespace nt::ui
