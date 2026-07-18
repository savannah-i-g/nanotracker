#include "app/undo.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

namespace nt::app {

void UndoStack::push(std::string label, std::function<void()> undo, std::function<void()> redo) {
    Entry entry{std::move(label), std::move(undo), std::move(redo)};
    if (group_depth_ > 0) {
        group_entries_.push_back(std::move(entry));
        // Group members are new edits too: the redo branch dies now,
        // not at commit time.
        redo_stack_.clear();
        return;
    }
    commit(std::move(entry));
}

void UndoStack::push_sample_op(std::string label, std::function<void()> undo,
                               std::function<void()> redo) {
    if (group_depth_ > 0) {
        // Composite entries are uncapped; the sample-op budget only
        // tracks flat entries (see the header note).
        push(std::move(label), std::move(undo), std::move(redo));
        return;
    }
    commit(Entry{std::move(label), std::move(undo), std::move(redo), /*sample_op=*/true});
}

void UndoStack::begin_group(std::string label) {
    if (group_depth_ == 0) {
        group_label_ = std::move(label);
    }
    ++group_depth_;
}

void UndoStack::end_group() {
    if (group_depth_ == 0) {
        return;
    }
    --group_depth_;
    if (group_depth_ > 0) {
        return;
    }
    if (group_entries_.empty()) {
        return;
    }
    if (group_entries_.size() == 1) {
        // Single member: keep it flat, relabelled for the group.
        Entry only = std::move(group_entries_.front());
        group_entries_.clear();
        only.label = std::move(group_label_);
        commit(std::move(only));
        return;
    }
    // Composite entry: undo unwinds members in reverse push order,
    // redo replays them forward. Shared ownership lets both closures
    // reference one member list.
    auto members = std::make_shared<std::vector<Entry>>(std::move(group_entries_));
    group_entries_.clear();
    commit({std::move(group_label_),
            [members] {
                for (const Entry& member : std::views::reverse(*members)) {
                    member.undo();
                }
            },
            [members] {
                for (const Entry& member : *members) {
                    member.redo();
                }
            }});
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
    group_entries_.clear();
    group_depth_ = 0;
}

void UndoStack::commit(Entry entry) {
    undo_stack_.push_back(std::move(entry));
    if (undo_stack_.size() > kMaxEntries) {
        undo_stack_.pop_front();
    }
    // Sample ops hold whole-buffer snapshots, so their depth caps
    // separately: evicting the oldest one truncates everything older
    // with it, keeping history a contiguous suffix (a mid-stack hole
    // would let undo reconstruct states that never existed).
    const auto sample_ops = static_cast<std::size_t>(std::count_if(
        undo_stack_.begin(), undo_stack_.end(), [](const Entry& e) { return e.sample_op; }));
    if (sample_ops > kMaxSampleOps) {
        while (!undo_stack_.empty()) {
            const bool was_sample_op = undo_stack_.front().sample_op;
            undo_stack_.pop_front();
            if (was_sample_op) {
                break;
            }
        }
    }
    // A new edit invalidates the redo branch.
    redo_stack_.clear();
}

} // namespace nt::app
