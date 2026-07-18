// Pattern block operations and undo grouping. The audio engine is
// constructed but never started: commands queue harmlessly, keeping
// the tests device-independent (same setup as session_test).
#include "app/pattern_ops.h"
#include "app/undo.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

using nt::app::CellField;
using nt::app::CellSelection;
using nt::engine::TrackerCell;

TrackerCell make_cell(int note, int instrument = 0, int volume = 0xFF, int effect = 0,
                      int effect_param = 0) {
    TrackerCell cell;
    cell.note = static_cast<std::uint8_t>(note);
    cell.instrument = static_cast<std::uint8_t>(instrument);
    cell.volume = static_cast<std::uint8_t>(volume);
    cell.effect = static_cast<std::uint8_t>(effect);
    cell.effect_param = static_cast<std::uint8_t>(effect_param);
    return cell;
}

const TrackerCell& cell_at(const nt::app::ProjectSession& session, int row, int channel) {
    return session.project().patterns[0].rows[static_cast<std::size_t>(row)]
                                             [static_cast<std::size_t>(channel)];
}

bool same_cell(const TrackerCell& a, const TrackerCell& b) {
    return a.note == b.note && a.instrument == b.instrument && a.volume == b.volume &&
           a.effect == b.effect && a.effect_param == b.effect_param &&
           a.bound_index == b.bound_index;
}

std::string undo_label(nt::app::ProjectSession& session) {
    return session.undo().next_undo_label();
}

} // namespace

TEST_CASE("pattern block operations", "[pattern_ops]") {
    nt::audio::AudioEngine audio; // not started — device-independent
    nt::app::ProjectSession session(audio);

    // 2x2 source block at rows 4-5, channels 1-2.
    const TrackerCell c00 = make_cell(49, 1, 0x20, 0xA, 0x0F);
    const TrackerCell c01 = make_cell(52, 2, 0x30);
    const TrackerCell c10 = make_cell(nt::engine::kNoteOff);
    const TrackerCell c11 = make_cell(61, 3, 0xFF, 0xC, 0x40);
    session.set_cell(0, 4, 1, c00);
    session.set_cell(0, 4, 2, c01);
    session.set_cell(0, 5, 1, c10);
    session.set_cell(0, 5, 2, c11);
    const CellSelection block{.pattern = 0, .row0 = 4, .row1 = 5, .channel0 = 1, .channel1 = 2};

    SECTION("copy/paste round-trip") {
        const auto clip = nt::app::copy_cells(session, block);
        REQUIRE(clip.rows == 2);
        REQUIRE(clip.channels == 2);

        nt::app::paste_cells(session, clip, 0, 10, 2);
        CHECK(same_cell(cell_at(session, 10, 2), c00));
        CHECK(same_cell(cell_at(session, 10, 3), c01));
        CHECK(same_cell(cell_at(session, 11, 2), c10));
        CHECK(same_cell(cell_at(session, 11, 3), c11));
        CHECK(same_cell(cell_at(session, 4, 1), c00)); // source untouched

        CHECK(undo_label(session) == "paste cells");
        REQUIRE(session.undo().undo());
        CHECK(same_cell(cell_at(session, 10, 2), TrackerCell{}));
        CHECK(same_cell(cell_at(session, 11, 3), TrackerCell{}));
        REQUIRE(session.undo().redo());
        CHECK(same_cell(cell_at(session, 11, 3), c11));
    }

    SECTION("paste clips at the pattern edges") {
        const auto clip = nt::app::copy_cells(session, block);
        nt::app::paste_cells(session, clip, 0, 63, 3); // bottom-right corner
        CHECK(same_cell(cell_at(session, 63, 3), c00));

        nt::app::paste_cells(session, clip, 0, -1, 0); // top edge: first row clipped
        CHECK(same_cell(cell_at(session, 0, 0), c10));
        CHECK(same_cell(cell_at(session, 0, 1), c11));
    }

    SECTION("reversed selection coordinates normalize") {
        const CellSelection reversed{.pattern = 0,
                                     .row0 = 5,
                                     .row1 = 4,
                                     .channel0 = 2,
                                     .channel1 = 1,
                                     .field0 = CellField::kEffect,
                                     .field1 = CellField::kNote};
        const auto clip = nt::app::copy_cells(session, reversed);
        REQUIRE(clip.rows == 2);
        REQUIRE(clip.channels == 2);
        CHECK(same_cell(clip.cells[0], c00));
    }

    SECTION("column-masked copy pastes only captured fields") {
        const CellSelection volume_only{.pattern = 0,
                                        .row0 = 4,
                                        .row1 = 4,
                                        .channel0 = 1,
                                        .channel1 = 1,
                                        .field0 = CellField::kVolume,
                                        .field1 = CellField::kVolume};
        const auto clip = nt::app::copy_cells(session, volume_only);
        nt::app::paste_cells(session, clip, 0, 5, 2); // onto c11
        const TrackerCell& result = cell_at(session, 5, 2);
        CHECK(result.volume == 0x20); // captured column landed
        CHECK(result.note == 61);     // everything else survived
        CHECK(result.instrument == 3);
        CHECK(result.effect == 0xC);
    }

    SECTION("cut clears the block and one undo restores everything") {
        const auto clip = nt::app::cut_cells(session, block);
        REQUIRE(clip.rows == 2);
        CHECK(same_cell(cell_at(session, 4, 1), TrackerCell{}));
        CHECK(same_cell(cell_at(session, 4, 2), TrackerCell{}));
        CHECK(same_cell(cell_at(session, 5, 1), TrackerCell{}));
        CHECK(same_cell(cell_at(session, 5, 2), TrackerCell{}));

        CHECK(undo_label(session) == "cut cells");
        REQUIRE(session.undo().undo());
        CHECK(same_cell(cell_at(session, 4, 1), c00));
        CHECK(same_cell(cell_at(session, 4, 2), c01));
        CHECK(same_cell(cell_at(session, 5, 1), c10));
        CHECK(same_cell(cell_at(session, 5, 2), c11));
        CHECK(undo_label(session) == "cell edit"); // back to the seeding edits
    }

    SECTION("clear respects the selection's field span") {
        const CellSelection tail{.pattern = 0,
                                 .row0 = 4,
                                 .row1 = 4,
                                 .channel0 = 1,
                                 .channel1 = 1,
                                 .field0 = CellField::kVolume,
                                 .field1 = CellField::kEffect};
        nt::app::clear_cells(session, tail);
        const TrackerCell& result = cell_at(session, 4, 1);
        CHECK(result.note == 49);
        CHECK(result.instrument == 1);
        CHECK(result.volume == 0xFF);
        CHECK(result.effect == 0);
        CHECK(result.effect_param == 0);
    }

    SECTION("edge channels trim to their fields, interior spans fully") {
        // channel 1 from the volume column on; channel 2 note only.
        const CellSelection span{.pattern = 0,
                                 .row0 = 4,
                                 .row1 = 4,
                                 .channel0 = 1,
                                 .channel1 = 2,
                                 .field0 = CellField::kVolume,
                                 .field1 = CellField::kNote};
        nt::app::clear_cells(session, span);
        CHECK(cell_at(session, 4, 1).note == 49);     // left edge keeps note+inst
        CHECK(cell_at(session, 4, 1).volume == 0xFF); // ...but loses volume+fx
        CHECK(cell_at(session, 4, 1).effect == 0);
        CHECK(cell_at(session, 4, 2).note == 0); // right edge loses note only
        CHECK(cell_at(session, 4, 2).instrument == 2);
        CHECK(cell_at(session, 4, 2).volume == 0x30);
    }

    SECTION("transpose shifts notes, clamps, and skips non-notes") {
        session.set_cell(0, 6, 1, make_cell(90, 1));
        const CellSelection wide{.pattern = 0, .row0 = 4, .row1 = 6, .channel0 = 1, .channel1 = 2};

        nt::app::transpose_cells(session, wide, 12);
        CHECK(cell_at(session, 4, 1).note == 61);
        CHECK(cell_at(session, 4, 2).note == 64);
        CHECK(cell_at(session, 5, 1).note == nt::engine::kNoteOff); // note-off untouched
        CHECK(cell_at(session, 6, 1).note == 96);                   // clamped high
        CHECK(cell_at(session, 6, 2).note == 0);                    // empty untouched
        CHECK(cell_at(session, 4, 1).instrument == 1);              // other fields ride along

        CHECK(undo_label(session) == "transpose cells");
        REQUIRE(session.undo().undo());
        CHECK(cell_at(session, 4, 1).note == 49);
        CHECK(cell_at(session, 6, 1).note == 90);

        nt::app::transpose_cells(session, wide, -60);
        CHECK(cell_at(session, 4, 1).note == 1); // clamped low
    }

    SECTION("interpolate fills volume and effect-param columns") {
        session.set_cell(0, 20, 0, make_cell(49, 1, 0x00));
        session.set_cell(0, 24, 0, make_cell(0, 0, 0x40));
        const CellSelection vol_span{
            .pattern = 0, .row0 = 20, .row1 = 24, .channel0 = 0, .channel1 = 0};
        REQUIRE(nt::app::interpolate_cells(session, vol_span));
        CHECK(cell_at(session, 21, 0).volume == 0x10);
        CHECK(cell_at(session, 22, 0).volume == 0x20);
        CHECK(cell_at(session, 23, 0).volume == 0x30);
        CHECK(cell_at(session, 20, 0).volume == 0x00); // endpoints keep their values
        CHECK(cell_at(session, 21, 0).note == 0);      // other fields untouched

        CHECK(undo_label(session) == "interpolate cells");
        REQUIRE(session.undo().undo());
        CHECK(cell_at(session, 22, 0).volume == 0xFF);

        // Effect-param fill copies the endpoint command onto the rows.
        session.set_cell(0, 30, 0, make_cell(0, 0, 0xFF, 0x1, 0x00));
        session.set_cell(0, 33, 0, make_cell(0, 0, 0xFF, 0x1, 0x40));
        const CellSelection fx_span{
            .pattern = 0, .row0 = 30, .row1 = 33, .channel0 = 0, .channel1 = 0};
        REQUIRE(nt::app::interpolate_cells(session, fx_span));
        CHECK(cell_at(session, 31, 0).effect == 0x1);
        CHECK(cell_at(session, 31, 0).effect_param == 0x15); // lround(64/3)
        CHECK(cell_at(session, 32, 0).effect_param == 0x2B); // lround(128/3)
    }

    SECTION("interpolate refuses unset or mismatched endpoints") {
        // Default volumes (0xFF) at both ends of an empty span.
        const CellSelection empty_span{
            .pattern = 0, .row0 = 40, .row1 = 44, .channel0 = 0, .channel1 = 0};
        CHECK_FALSE(nt::app::interpolate_cells(session, empty_span));

        // Different effect commands at the endpoints.
        session.set_cell(0, 40, 0, make_cell(0, 0, 0xFF, 0x1, 0x10));
        session.set_cell(0, 44, 0, make_cell(0, 0, 0xFF, 0x2, 0x40));
        CHECK_FALSE(nt::app::interpolate_cells(session, empty_span));
        CHECK(cell_at(session, 42, 0).effect == 0);
    }

    SECTION("no-op operations leave no undo entry") {
        nt::app::ProjectSession fresh(audio);
        const CellSelection empty{.pattern = 0, .row0 = 0, .row1 = 3, .channel0 = 0, .channel1 = 1};
        nt::app::transpose_cells(fresh, empty, 12); // nothing to move
        nt::app::clear_cells(fresh, empty);         // already clear
        CHECK_FALSE(nt::app::interpolate_cells(fresh, empty));
        CHECK_FALSE(fresh.undo().can_undo());
    }

    SECTION("selections outside the pattern are rejected") {
        const CellSelection bad{.pattern = 7, .row0 = 0, .row1 = 1, .channel0 = 0, .channel1 = 0};
        CHECK(nt::app::copy_cells(session, bad).empty());
        const CellSelection off_grid{
            .pattern = 0, .row0 = 100, .row1 = 120, .channel0 = 0, .channel1 = 0};
        CHECK(nt::app::copy_cells(session, off_grid).empty());
    }
}

TEST_CASE("undo groups collapse batched edits", "[undo]") {
    nt::app::UndoStack stack;
    int value = 0;
    const auto apply = [&stack, &value](int to, const char* label = "set") {
        const int before = value;
        value = to;
        stack.push(label, [&value, before] { value = before; }, [&value, to] { value = to; });
    };

    SECTION("N edits inside a group pop with one undo") {
        {
            const nt::app::UndoGroup group(stack, "batch");
            apply(1);
            apply(2);
            apply(3);
        }
        CHECK(std::string(stack.next_undo_label()) == "batch");
        REQUIRE(stack.undo());
        CHECK(value == 0);
        CHECK_FALSE(stack.can_undo()); // the three edits were one entry
        REQUIRE(stack.redo());
        CHECK(value == 3);
    }

    SECTION("nested groups commit at the outermost close") {
        stack.begin_group("outer");
        apply(1);
        stack.begin_group("inner");
        apply(2);
        stack.end_group();
        apply(3);
        stack.end_group();
        CHECK(std::string(stack.next_undo_label()) == "outer");
        REQUIRE(stack.undo());
        CHECK(value == 0);
        CHECK_FALSE(stack.can_undo());
    }

    SECTION("empty groups commit nothing") {
        { const nt::app::UndoGroup group(stack, "nothing"); }
        CHECK_FALSE(stack.can_undo());
    }

    SECTION("single-member groups stay flat under the group label") {
        {
            const nt::app::UndoGroup group(stack, "one");
            apply(5);
        }
        CHECK(std::string(stack.next_undo_label()) == "one");
        REQUIRE(stack.undo());
        CHECK(value == 0);
    }

    SECTION("ungrouped pushes still work") {
        apply(1);
        apply(2);
        REQUIRE(stack.undo());
        CHECK(value == 1);
        REQUIRE(stack.undo());
        CHECK(value == 0);
        CHECK_FALSE(stack.undo());
    }
}
