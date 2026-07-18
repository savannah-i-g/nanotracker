#include "app/undo.h"

#include <utility>

namespace nt::app {

void UndoStack::push(std::string label, std::function<void()> undo, std::function<void()> redo) {
    undo_stack_.push_back({std::move(label), std::move(undo), std::move(redo)});
    if (undo_stack_.size() > kMaxEntries) {
        undo_stack_.pop_front();
    }
    // A new edit invalidates the redo branch.
    redo_stack_.clear();
}

bool UndoStack::undo() {
    if (undo_stack_.empty()) {
        return false;
    }
    Entry entry = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    entry.undo();
    redo_stack_.push_back(std::move(entry));
    return true;
}

bool UndoStack::redo() {
    if (redo_stack_.empty()) {
        return false;
    }
    Entry entry = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    entry.redo();
    undo_stack_.push_back(std::move(entry));
    return true;
}

const char* UndoStack::next_undo_label() const {
    return undo_stack_.empty() ? "" : undo_stack_.back().label.c_str();
}

const char* UndoStack::next_redo_label() const {
    return redo_stack_.empty() ? "" : redo_stack_.back().label.c_str();
}

void UndoStack::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

} // namespace nt::app
