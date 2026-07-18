// app/undo — the command/undo spine.
// Every editing surface routes mutations through an UndoStack so undo
// and redo work uniformly across the application. Entries carry
// closures over the exact inverse operations rather than state
// snapshots (the web app's React-snapshot approach does not port).
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nt::app {

class UndoStack {
public:
    // Registers an already-applied mutation. `undo`/`redo` must be
    // self-contained closures; they run on the UI thread. While a
    // group is open the entry joins the group instead of the stack.
    void push(std::string label, std::function<void()> undo, std::function<void()> redo);

    // Like push, but the entry counts against kMaxSampleOps instead of
    // only the overall cap: sample-op closures snapshot whole audio
    // buffers, and that memory is the honest cost of destructive-edit
    // undo. Exceeding the cap evicts the oldest sample op AND every
    // entry older than it, so history stays a contiguous suffix of the
    // edit sequence (the same truncation shape as the overall cap).
    // Not for use inside a group — a grouped push degrades to a normal
    // (uncapped) member.
    void push_sample_op(std::string label, std::function<void()> undo, std::function<void()> redo);

    // Collapses every push between begin_group and the matching
    // end_group into one composite entry under `label` (block edits
    // undo as a unit). Groups nest: only the outermost end_group
    // commits, and an empty group commits nothing. undo()/redo() must
    // not run while a group is open. Prefer the UndoGroup RAII scope.
    void begin_group(std::string label);
    void end_group();

    // Applies one step; returns false when the respective stack is empty.
    bool undo();
    bool redo();

    [[nodiscard]] bool can_undo() const { return !undo_stack_.empty(); }

    [[nodiscard]] bool can_redo() const { return !redo_stack_.empty(); }

    [[nodiscard]] const char* next_undo_label() const;
    [[nodiscard]] const char* next_redo_label() const;

    // Wipes all history because a structural edit outside the undo model
    // reshaped the project (project_session's structural ops — see its
    // header). Leaves a breadcrumb for the HISTORY panel: was_cleared()
    // reads true and clear_reason() names the wipe until the next
    // undoable edit, or a reset(), supersedes it. The default reason
    // matches the panel's generic surface; callers with a more specific
    // cause pass it.
    void clear(std::string reason = "structural edit");

    // Empties history for a project load / new project. Unlike clear(),
    // leaves no breadcrumb — a fresh project never lost history, so the
    // panel reads as the empty state rather than a wipe.
    void reset();

    // ── HISTORY panel read surface ───────────────────────────────────
    // Read-only enumeration for ui/history_view, allocation-free: entry
    // labels alias the live storage (valid until the next stack
    // mutation, exactly like next_undo_label()).

    // One visible edit: its label and whether it is a bounded buffer-
    // snapshot op (push_sample_op), which the panel tags.
    struct HistoryEntry {
        const char* label = "";
        bool sample_op = false;
    };

    // Done edits sit above the current position; the redo branch below.
    [[nodiscard]] std::size_t undo_depth() const { return undo_stack_.size(); }

    [[nodiscard]] std::size_t redo_depth() const { return redo_stack_.size(); }

    // Buffer-snapshot entries held against kMaxSampleOps (the undo side,
    // where the cap is enforced).
    [[nodiscard]] std::size_t sample_op_depth() const;

    // Linear-list access. undo_at indexes oldest→newest (0 is the oldest
    // still-undoable edit; undo_depth()-1 is the next undo). redo_at
    // indexes in replay order (0 is the next redo). Out-of-range yields a
    // default (empty-label) entry.
    [[nodiscard]] HistoryEntry undo_at(std::size_t index) const;
    [[nodiscard]] HistoryEntry redo_at(std::size_t index) const;

    // True while a structural edit's wipe is the latest history event —
    // reset() and the next undoable edit clear it; clear_reason() then
    // names the wipe.
    [[nodiscard]] bool was_cleared() const { return cleared_; }

    [[nodiscard]] const char* clear_reason() const { return clear_reason_.c_str(); }

    // Depth cap for buffer-snapshot entries (push_sample_op).
    static constexpr std::size_t kMaxSampleOps = 8;

    // Bounded history: oldest entries fall off the front past this cap.
    // Public for the panel's depth indicator.
    static constexpr std::size_t kMaxEntries = 512;

private:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
        bool sample_op = false;
    };

    void commit(Entry entry);
    // Empties both stacks and any open-group state — the shared core of
    // clear() and reset(); only the breadcrumb handling differs.
    void discard_all();

    std::deque<Entry> undo_stack_;
    std::deque<Entry> redo_stack_;

    // Open-group state: pushes accumulate here until the outermost
    // end_group commits them as one entry.
    std::vector<Entry> group_entries_;
    std::string group_label_;
    int group_depth_ = 0;

    // HISTORY-panel breadcrumb: set by clear(reason); cleared by reset()
    // and by the next undoable edit (commit / grouped push).
    bool cleared_ = false;
    std::string clear_reason_;
};

// Scope guard pairing begin_group/end_group around a block operation.
class UndoGroup {
public:
    UndoGroup(UndoStack& stack, std::string label) : stack_(stack) {
        stack_.begin_group(std::move(label));
    }

    ~UndoGroup() { stack_.end_group(); }

    UndoGroup(const UndoGroup&) = delete;
    UndoGroup& operator=(const UndoGroup&) = delete;
    UndoGroup(UndoGroup&&) = delete;
    UndoGroup& operator=(UndoGroup&&) = delete;

private:
    UndoStack& stack_;
};

} // namespace nt::app
