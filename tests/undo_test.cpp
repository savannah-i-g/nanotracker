// The HISTORY panel read surface (Stage 26): enumeration order, depths,
// the bounded sample-op budget, group collapsing, and the structural-
// clear breadcrumb. The closures are trivial no-ops here — these assert
// the accessors, not the edits they would apply.
#include "app/undo.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using nt::app::UndoStack;

// A no-op edit: the read accessors never run the closures.
void push_edit(UndoStack& stack, const std::string& label) {
    stack.push(label, [] {}, [] {});
}

void push_sample_edit(UndoStack& stack, const std::string& label) {
    stack.push_sample_op(label, [] {}, [] {});
}

} // namespace

TEST_CASE("history enumerates undo entries oldest to newest", "[undo]") {
    UndoStack stack;
    push_edit(stack, "a");
    push_edit(stack, "b");
    push_edit(stack, "c");

    REQUIRE(stack.undo_depth() == 3);
    REQUIRE(stack.redo_depth() == 0);
    CHECK(std::string(stack.undo_at(0).label) == "a");
    CHECK(std::string(stack.undo_at(1).label) == "b");
    CHECK(std::string(stack.undo_at(2).label) == "c");
    CHECK(stack.undo_at(3).label[0] == '\0'); // out of range: default entry
    CHECK_FALSE(stack.undo_at(0).sample_op);
    CHECK_FALSE(stack.was_cleared());
}

TEST_CASE("undo moves the split; redo enumerates in replay order", "[undo]") {
    UndoStack stack;
    push_edit(stack, "a");
    push_edit(stack, "b");
    push_edit(stack, "c");

    REQUIRE(stack.undo());
    REQUIRE(stack.undo());
    CHECK(stack.undo_depth() == 1);
    CHECK(stack.redo_depth() == 2);
    CHECK(std::string(stack.undo_at(0).label) == "a");
    // Replay order: the next redo comes first.
    CHECK(std::string(stack.redo_at(0).label) == "b");
    CHECK(std::string(stack.redo_at(1).label) == "c");
    CHECK(stack.redo_at(2).label[0] == '\0'); // out of range
}

TEST_CASE("a committed group is one history entry", "[undo]") {
    UndoStack stack;
    push_edit(stack, "before");
    stack.begin_group("block edit");
    push_edit(stack, "m1");
    push_edit(stack, "m2");
    push_edit(stack, "m3");
    stack.end_group();

    CHECK(stack.undo_depth() == 2);
    CHECK(std::string(stack.undo_at(1).label) == "block edit");
}

TEST_CASE("sample-op depth reflects the bounded budget", "[undo]") {
    UndoStack stack;
    for (int i = 0; i < 8; ++i) {
        push_sample_edit(stack, "op" + std::to_string(i));
    }
    CHECK(stack.undo_depth() == 8);
    CHECK(stack.sample_op_depth() == 8);
    CHECK(std::string(stack.undo_at(0).label) == "op0");
    CHECK(stack.undo_at(0).sample_op);

    // The ninth evicts the oldest sample op.
    push_sample_edit(stack, "op8");
    CHECK(stack.undo_depth() == 8);
    CHECK(stack.sample_op_depth() == 8);
    CHECK(std::string(stack.undo_at(0).label) == "op1"); // op0 fell off
    CHECK(std::string(stack.undo_at(7).label) == "op8");
}

TEST_CASE("sample-op eviction truncates older non-sample entries", "[undo]") {
    UndoStack stack;
    push_edit(stack, "flat"); // oldest, not a sample op
    for (int i = 0; i < 8; ++i) {
        push_sample_edit(stack, "op" + std::to_string(i));
    }
    REQUIRE(stack.undo_depth() == 9);
    REQUIRE(stack.sample_op_depth() == 8);

    // Ninth sample op: budget exceeded → drop from the front until a
    // sample op is dropped, so "flat" and "op0" both go with it.
    push_sample_edit(stack, "op8");
    CHECK(stack.undo_depth() == 8);
    CHECK(stack.sample_op_depth() == 8);
    CHECK(std::string(stack.undo_at(0).label) == "op1");
}

TEST_CASE("clear leaves a breadcrumb; reset and edits do not", "[undo]") {
    UndoStack stack;
    push_edit(stack, "a");

    stack.clear("pattern added");
    CHECK(stack.undo_depth() == 0);
    CHECK(stack.redo_depth() == 0);
    CHECK(stack.was_cleared());
    CHECK(std::string(stack.clear_reason()) == "pattern added");

    // A fresh undoable edit supersedes the breadcrumb.
    push_edit(stack, "b");
    CHECK_FALSE(stack.was_cleared());

    // reset() empties without a breadcrumb (project load / new project).
    stack.clear("structural edit");
    REQUIRE(stack.was_cleared());
    stack.reset();
    CHECK_FALSE(stack.was_cleared());
    CHECK(stack.undo_depth() == 0);

    // The default reason matches the panel's generic surface.
    stack.clear();
    CHECK(std::string(stack.clear_reason()) == "structural edit");
}
