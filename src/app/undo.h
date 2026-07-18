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

namespace nt::app {

class UndoStack {
public:
    // Registers an already-applied mutation. `undo`/`redo` must be
    // self-contained closures; they run on the UI thread.
    void push(std::string label, std::function<void()> undo, std::function<void()> redo);

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

    std::deque<Entry> undo_stack_;
    std::deque<Entry> redo_stack_;
};

} // namespace nt::app
