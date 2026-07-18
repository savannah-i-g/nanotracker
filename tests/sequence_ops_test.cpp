// Pure sequence transforms: exact outputs, seeded determinism, and
// input-order preservation (results map back onto selection indices).
#include "engine/sequence_ops.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

namespace {

using nt::engine::SequenceNote;

SequenceNote note(int pitch, int start, int duration, int velocity = 100) {
    return {.pitch = pitch, .start_tick = start, .duration_ticks = duration, .velocity = velocity};
}

} // namespace

TEST_CASE("quantize snaps starts and durations by strength", "[sequence_ops]") {
    const std::vector<SequenceNote> notes = {note(60, 4, 4), note(64, 7, 2), note(67, 2, 8)};

    SECTION("full strength, whole-row grid") {
        const auto out = nt::engine::quantize(notes, 6, 1, 100);
        REQUIRE(out.size() == 3);
        CHECK(out[0].start_tick == 6);
        CHECK(out[1].start_tick == 6);
        CHECK(out[2].start_tick == 0);
        CHECK(out[0].duration_ticks == 4); // untouched without the flag
    }

    SECTION("half strength moves halfway to the grid") {
        const auto out = nt::engine::quantize(notes, 6, 1, 50);
        CHECK(out[0].start_tick == 5); // 4 + (6-4)/2
    }

    SECTION("half-row grid") {
        const auto out = nt::engine::quantize(notes, 6, 2, 100);
        CHECK(out[0].start_tick == 3);
        CHECK(out[1].start_tick == 6);
        CHECK(out[2].start_tick == 3);
    }

    SECTION("duration quantize snaps to at least one grid step") {
        const auto out = nt::engine::quantize(notes, 6, 1, 100, true);
        CHECK(out[0].duration_ticks == 6); // 4 -> one row
        CHECK(out[1].duration_ticks == 6); // 2 -> minimum one row
        CHECK(out[2].duration_ticks == 6); // 8 rounds down to one row
    }
}

TEST_CASE("humanize jitters deterministically from a seed", "[sequence_ops]") {
    const std::vector<SequenceNote> notes = {note(60, 0, 6), note(64, 6, 6), note(67, 12, 6)};

    SECTION("exact outputs for seed 1234 at default amounts") {
        std::mt19937 rng(1234);
        const auto out = nt::engine::humanize(notes, 6, rng);
        REQUIRE(out.size() == 3);
        CHECK(out[0].start_tick == 0); // -1 tick of jitter clamps at 0
        CHECK(out[0].velocity == 100);
        CHECK(out[0].duration_ticks == 6);
        CHECK(out[1].start_tick == 7);
        CHECK(out[1].velocity == 98);
        CHECK(out[1].duration_ticks == 6);
        CHECK(out[2].start_tick == 13);
        CHECK(out[2].velocity == 108);
        CHECK(out[2].duration_ticks == 7);
        CHECK(out[0].pitch == 60); // pitch never jitters
    }

    SECTION("same seed reproduces, different seed diverges") {
        std::mt19937 a(7);
        std::mt19937 b(7);
        std::mt19937 c(8);
        const auto out_a = nt::engine::humanize(notes, 6, a);
        const auto out_b = nt::engine::humanize(notes, 6, b);
        const auto out_c = nt::engine::humanize(notes, 6, c);
        for (std::size_t i = 0; i < notes.size(); ++i) {
            CHECK(out_a[i].start_tick == out_b[i].start_tick);
            CHECK(out_a[i].velocity == out_b[i].velocity);
            CHECK(out_a[i].duration_ticks == out_b[i].duration_ticks);
        }
        bool any_diff = false;
        for (std::size_t i = 0; i < notes.size(); ++i) {
            any_diff = any_diff || out_a[i].start_tick != out_c[i].start_tick ||
                       out_a[i].velocity != out_c[i].velocity ||
                       out_a[i].duration_ticks != out_c[i].duration_ticks;
        }
        CHECK(any_diff);
    }

    SECTION("zero amounts are the identity") {
        std::mt19937 rng(1);
        const auto out = nt::engine::humanize(notes, 6, rng, 0, 0, 0);
        for (std::size_t i = 0; i < notes.size(); ++i) {
            CHECK(out[i].start_tick == notes[i].start_tick);
            CHECK(out[i].velocity == notes[i].velocity);
            CHECK(out[i].duration_ticks == notes[i].duration_ticks);
        }
    }
}

TEST_CASE("transpose shifts and clamps pitch", "[sequence_ops]") {
    const auto out = nt::engine::transpose({note(60, 0, 6), note(125, 6, 6), note(5, 12, 6)}, 7);
    CHECK(out[0].pitch == 67);
    CHECK(out[1].pitch == 127); // clamped high
    CHECK(out[2].pitch == 12);
    const auto down = nt::engine::transpose({note(5, 0, 6)}, -12);
    CHECK(down[0].pitch == 0); // clamped low
}

TEST_CASE("reverse mirrors the block in time", "[sequence_ops]") {
    const auto out = nt::engine::reverse({note(60, 0, 6), note(64, 6, 6), note(67, 12, 3)});
    REQUIRE(out.size() == 3);
    // Span is [0, 15]; each note's end maps to the mirrored start.
    CHECK(out[0].start_tick == 9);
    CHECK(out[1].start_tick == 3);
    CHECK(out[2].start_tick == 0);
    CHECK(out[0].pitch == 60); // pitches ride along in input order

    const auto single = nt::engine::reverse({note(60, 4, 6)});
    CHECK(single[0].start_tick == 4);
}

TEST_CASE("invert mirrors pitches around the selection center", "[sequence_ops]") {
    const auto out = nt::engine::invert({note(60, 0, 6), note(64, 6, 6), note(67, 12, 6)});
    // Center of [60, 67] rounds to 64.
    CHECK(out[0].pitch == 68);
    CHECK(out[1].pitch == 64);
    CHECK(out[2].pitch == 61);

    const auto clamped = nt::engine::invert({note(0, 0, 6), note(127, 6, 6)});
    CHECK(clamped[0].pitch == 127); // 2*64-0 = 128 clamps
    CHECK(clamped[1].pitch == 1);

    const auto single = nt::engine::invert({note(60, 0, 6)});
    CHECK(single[0].pitch == 60);
}

TEST_CASE("arpeggiate fans a chord into a run", "[sequence_ops]") {
    std::mt19937 rng(42);
    const std::vector<SequenceNote> chord = {note(60, 0, 24, 100), note(64, 0, 24, 90),
                                             note(67, 0, 24, 110)};

    SECTION("up direction cycles ascending pitches") {
        const auto out = nt::engine::arpeggiate(chord, 6, nt::engine::ArpDirection::kUp, 1, rng);
        REQUIRE(out.size() == 4);
        CHECK(out[0].pitch == 60);
        CHECK(out[1].pitch == 64);
        CHECK(out[2].pitch == 67);
        CHECK(out[3].pitch == 60); // cycle wraps
        for (std::size_t i = 0; i < out.size(); ++i) {
            CHECK(out[i].start_tick == static_cast<int>(i) * 6);
            CHECK(out[i].duration_ticks == 4); // 80% gate of a 6-tick step
            CHECK(out[i].velocity == 100);     // mean of 100/90/110
        }
    }

    SECTION("down and updown directions") {
        const auto down = nt::engine::arpeggiate(chord, 6, nt::engine::ArpDirection::kDown, 1, rng);
        CHECK(down[0].pitch == 67);
        CHECK(down[1].pitch == 64);
        CHECK(down[2].pitch == 60);
        const auto updown =
            nt::engine::arpeggiate(chord, 6, nt::engine::ArpDirection::kUpDown, 1, rng);
        // Cycle 60 64 67 64 over four steps.
        CHECK(updown[3].pitch == 64);
    }

    SECTION("random direction shuffles once, deterministically") {
        std::mt19937 seeded(42);
        const auto out =
            nt::engine::arpeggiate(chord, 6, nt::engine::ArpDirection::kRandom, 1, seeded);
        REQUIRE(out.size() == 4);
        CHECK(out[0].pitch == 67);
        CHECK(out[1].pitch == 64);
        CHECK(out[2].pitch == 60);
        CHECK(out[3].pitch == 67); // same shuffled cycle repeats
    }

    SECTION("octave range expands the pitch pool") {
        const auto out =
            nt::engine::arpeggiate(chord, 6, nt::engine::ArpDirection::kUp, 1, rng, 80, 2);
        REQUIRE(out.size() == 4);
        CHECK(out[3].pitch == 72); // 60 64 67 72 76 79 cycle
    }

    SECTION("fast rate subdivides the row and the last step clips") {
        const std::vector<SequenceNote> pair = {note(60, 0, 8, 100), note(64, 0, 8, 100)};
        const auto out = nt::engine::arpeggiate(pair, 6, nt::engine::ArpDirection::kUp, 1, rng);
        REQUIRE(out.size() == 2);
        CHECK(out[1].duration_ticks == 2); // span ends at 8, step starts at 6
        const auto fast = nt::engine::arpeggiate(pair, 6, nt::engine::ArpDirection::kUp, 8, rng);
        REQUIRE(fast.size() == 8); // step clamps to 1 tick, gate to 1
        CHECK(fast[0].duration_ticks == 1);
    }

    SECTION("empty selection yields nothing") {
        CHECK(nt::engine::arpeggiate({}, 6, nt::engine::ArpDirection::kUp, 1, rng).empty());
    }
}

TEST_CASE("velocity curve shapes by time rank, preserving order", "[sequence_ops]") {
    SECTION("crescendo and decrescendo ramp linearly") {
        const std::vector<SequenceNote> notes = {note(60, 0, 6), note(64, 6, 6), note(67, 12, 6)};
        const auto up =
            nt::engine::velocity_curve(notes, nt::engine::VelocityCurveShape::kCrescendo);
        CHECK(up[0].velocity == 30);
        CHECK(up[1].velocity == 75);
        CHECK(up[2].velocity == 120);
        const auto down =
            nt::engine::velocity_curve(notes, nt::engine::VelocityCurveShape::kDecrescendo);
        CHECK(down[0].velocity == 120);
        CHECK(down[2].velocity == 30);
    }

    SECTION("ranks follow time even when input order does not") {
        const std::vector<SequenceNote> scrambled = {note(64, 6, 6), note(60, 0, 6)};
        const auto out =
            nt::engine::velocity_curve(scrambled, nt::engine::VelocityCurveShape::kCrescendo);
        CHECK(out[0].pitch == 64); // input order kept
        CHECK(out[0].velocity == 120);
        CHECK(out[1].velocity == 30);
    }

    SECTION("accent hits every fourth note; flat levels all") {
        std::vector<SequenceNote> notes;
        for (int i = 0; i < 5; ++i) {
            notes.push_back(note(60, i * 6, 6));
        }
        const auto accent =
            nt::engine::velocity_curve(notes, nt::engine::VelocityCurveShape::kAccent);
        CHECK(accent[0].velocity == 120);
        CHECK(accent[1].velocity == 30);
        CHECK(accent[4].velocity == 120);
        const auto flat =
            nt::engine::velocity_curve(notes, nt::engine::VelocityCurveShape::kFlat, 80, 80);
        CHECK(flat[2].velocity == 80);
    }
}

TEST_CASE("gate length rescales and legato fills to the next start", "[sequence_ops]") {
    SECTION("staccato scales down with a floor of one tick") {
        const auto out = nt::engine::gate_length({note(60, 0, 6), note(64, 6, 1)}, 50);
        CHECK(out[0].duration_ticks == 3);
        CHECK(out[1].duration_ticks == 1);
    }

    SECTION("legato reaches the next later start regardless of pitch") {
        const auto out =
            nt::engine::gate_length({note(60, 0, 2), note(64, 6, 6), note(60, 12, 2)}, 100);
        CHECK(out[0].duration_ticks == 6);
        CHECK(out[1].duration_ticks == 6);
        CHECK(out[2].duration_ticks == 2); // nothing after: length kept
    }

    SECTION("chord-mates share one gap; overlap goes past it") {
        const auto legato =
            nt::engine::gate_length({note(60, 0, 2), note(64, 0, 3), note(72, 6, 2)}, 100);
        CHECK(legato[0].duration_ticks == 6);
        CHECK(legato[1].duration_ticks == 6);
        const auto overlap = nt::engine::gate_length({note(60, 0, 2), note(64, 6, 2)}, 200);
        CHECK(overlap[0].duration_ticks == 12);
        CHECK(overlap[1].duration_ticks == 4);
    }
}
