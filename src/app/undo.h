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

    void clear();

private:
    struct Entry {
        std::string label;
        std::function<void()> undo;
        std::function<void()> redo;
    };

    // Bounded history: oldest entries fall off the front.
    static constexpr std::size_t kMaxEntries = 512;

    void commit(Entry entry);

    std::deque<Entry> undo_stack_;
    std::deque<Entry> redo_stack_;

    // Open-group state: pushes accumulate here until the outermost
    // end_group commits them as one entry.
    std::vector<Entry> group_entries_;
    std::string group_label_;
    int group_depth_ = 0;
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
