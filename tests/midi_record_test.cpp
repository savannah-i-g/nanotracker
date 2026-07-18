// Pattern record + step entry verification (app/midi_record): the
// quantise rule as pure math (nearest row, halves up, boundary
// round-up, wrap), live record end to end through a cabled Ext MIDI
// In → master.midi.in ring on the deterministic offline engine,
// step-entry parity with the pattern editor's keyboard path, and the
// live ALSA loopback record round trip (WARN-skip on headless CI).
#include "app/midi_record.h"
#include "app/project_session.h"
#include "midi/midi_io.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// Cursor double standing in for ui::PatternView: fixed pattern/
// channel/slot, advance mirrors the keyboard edit-step (one row,
// clamped at the last).
class FakeCursor : public nt::app::PatternCursor {
public:
    int pattern = 0;
    int row = 0;
    int channel = 0;
    int slot = 1;
    int rows = 64;
    int advances = 0;

    [[nodiscard]] int step_pattern() const override { return pattern; }

    [[nodiscard]] int step_row() const override { return row; }

    [[nodiscard]] int step_channel() const override { return channel; }

    [[nodiscard]] int step_entry_slot() const override { return slot; }

    void step_advance() override {
        row = std::min(rows - 1, row + 1);
        ++advances;
    }
};

int find_port(const std::vector<std::string>& names, const std::string& needle) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].find(needle) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("record quantisation snaps events to the nearest row", "[midi_record]") {
    // 48 kHz, 125 BPM, speed 6: tick = 960 frames, row = 5760.
    nt::app::RecordAnchor anchor;
    anchor.row = 4;
    anchor.tick = 0;
    anchor.stream_frames = 1'000'000;

    // At the publish instant and through the first half of the row:
    // the anchor row itself.
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000) == 4);
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 2000) == 4);
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 2879) == 4);
    // Boundary round-up: the midpoint and the second half land on the
    // NEXT row.
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 2880) == 5);
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 3000) == 5);
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 5000) == 5);
    // Events before the publish instant (negative delta): nearest row
    // still, back into the previous row past its midpoint.
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 - 2000) == 4);
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 - 3000) == 3);

    // Tick anchoring: published mid-row (tick 3 of 6 = half a row in),
    // an event at the publish instant already rounds up.
    anchor.tick = 3;
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000) == 5);

    // Round-up past the final row wraps to row 0 of the pattern.
    anchor.row = 63;
    anchor.tick = 5;
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 500) == 0);

    // Guard inputs return the anchor row.
    anchor.row = 7;
    anchor.tick = 0;
    anchor.speed = 0;
    CHECK(nt::app::quantise_record_row(anchor, 1'000'000 + 50'000) == 7);
}

TEST_CASE("step entry writes the keyboard path and advances the cursor", "[midi_record]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);

    FakeCursor cursor;
    cursor.row = 5;
    cursor.slot = 2;
    nt::app::MidiRecord record(audio, cursor);
    record.set_mode(nt::app::MidiInputMode::kEnter);
    record.set_velocity_to_volume(false);

    // MIDI 60 → tracker C-4 (49) at the cursor; velocity mapping off
    // leaves the volume column exactly as keyboard entry does.
    record.on_device_note_on(session, 60, 100);
    CHECK(cursor.row == 6);
    CHECK(cursor.advances == 1);

    // The keyboard path's write for the same note at another row (the
    // exact call ui::PatternView makes for C-4 with slot 2).
    nt::app::enter_note_cell(session, 0, 20, 0, 49, 2, -1, true);

    const auto& rows = session.project().patterns[0].rows;
    const nt::engine::TrackerCell& entered = rows[5][0];
    const nt::engine::TrackerCell& keyboard = rows[20][0];
    CHECK(entered.note == 49);
    CHECK(entered.instrument == 2);
    CHECK(entered.volume == 0xFF);
    CHECK(entered.note == keyboard.note);
    CHECK(entered.instrument == keyboard.instrument);
    CHECK(entered.volume == keyboard.volume);
    CHECK(entered.effect == keyboard.effect);
    CHECK(entered.effect_param == keyboard.effect_param);

    // Velocity → volume on: round(127/127*64) = 64 at the advanced row.
    record.set_velocity_to_volume(true);
    record.on_device_note_on(session, 72, 127);
    CHECK(rows[6][0].note == 61);
    CHECK(rows[6][0].volume == 64);

    // Note-offs neither write nor advance in enter mode.
    record.on_device_note_off(session, 72);
    CHECK(cursor.row == 7);
    CHECK(cursor.advances == 2);

    // Every write routed through set_cell → the undo stack restores
    // (newest first: rows[6], rows[20], rows[5]).
    REQUIRE(session.undo().can_undo());
    session.undo().undo();
    session.undo().undo();
    session.undo().undo();
    CHECK(rows[5][0].note == 0);
    CHECK(rows[6][0].note == 0);
    CHECK(rows[20][0].note == 0);

    // Preview / off modes never write.
    record.set_mode(nt::app::MidiInputMode::kPreview);
    record.on_device_note_on(session, 60, 100);
    record.set_mode(nt::app::MidiInputMode::kOff);
    record.on_device_note_on(session, 60, 100);
    for (const auto& row : rows) {
        for (const auto& cell : row) {
            CHECK(cell.note == 0);
        }
    }
}

TEST_CASE("live record quantises cabled bus notes onto playhead rows", "[midi_record]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::rt::SpscQueue<nt::audio::MidiMessage> ring{64};
    audio.set_ext_midi_source(&ring);
    nt::app::ProjectSession session(audio);

    const std::string emi = session.add_ext_midi_in_node();
    REQUIRE_FALSE(emi.empty());
    REQUIRE(session.add_cable({.node_id = emi, .port_id = "midi"},
                              {.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi.in"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);

    FakeCursor cursor;
    cursor.slot = 5;
    nt::app::MidiRecord record(audio, cursor);
    record.set_mode(nt::app::MidiInputMode::kRecord);
    record.set_armed_channel(1);
    record.set_velocity_to_volume(true);

    std::array<float, 256> block{};
    audio.render_offline(block.data(), 128); // drain session publishes

    // Transport off: bus notes must not write.
    ring.push({.frame = 0,
               .type = nt::audio::MidiMessage::Type::kNoteOn,
               .channel = 0,
               .data1 = 60,
               .data2 = 100});
    audio.render_offline(block.data(), 128);
    record.update(session);
    const auto& rows = session.project().patterns[0].rows;
    for (const auto& row : rows) {
        CHECK(row[1].note == 0);
    }

    // Default project: 125 BPM speed 6 at 48 kHz → row = 5760 frames.
    // Play starts at the next block (stream 256 after the two setup
    // blocks), so row r begins at 256 + r·5760.
    session.play();
    for (int i = 0; i < 90; ++i) {
        audio.render_offline(block.data(), 128);
    }
    // Ext MIDI In stamps arrivals at the start of the draining block:
    // block 92 starts at 11776 = 256 + 2·5760 — exactly row 2.
    ring.push({.frame = 0,
               .type = nt::audio::MidiMessage::Type::kNoteOn,
               .channel = 0,
               .data1 = 60,
               .data2 = 100});
    audio.render_offline(block.data(), 128);
    record.update(session);
    CHECK(rows[2][1].note == 49); // MIDI 60 → C-4
    CHECK(rows[2][1].instrument == 5);
    CHECK(rows[2][1].volume == 50); // round(100/127·64)

    // Boundary round-up: block 122 starts at 15616 = row 2 start +
    // 3840 frames = past the 2880-frame midpoint (with a full tick of
    // margin over the anchor's sub-tick slack) → row 3.
    for (int i = 0; i < 29; ++i) {
        audio.render_offline(block.data(), 128);
    }
    ring.push({.frame = 0,
               .type = nt::audio::MidiMessage::Type::kNoteOn,
               .channel = 0,
               .data1 = 62,
               .data2 = 64});
    audio.render_offline(block.data(), 128);
    record.update(session);
    CHECK(rows[3][1].note == 51); // MIDI 62
    CHECK(rows[3][1].instrument == 5);
    CHECK(rows[3][1].volume == 32); // round(64/127·64)

    // Note-offs never write cells (web record semantics).
    ring.push({.frame = 0,
               .type = nt::audio::MidiMessage::Type::kNoteOff,
               .channel = 0,
               .data1 = 60,
               .data2 = 0});
    audio.render_offline(block.data(), 128);
    record.update(session);
    session.stop();
    int written = 0;
    for (const auto& row : rows) {
        if (row[1].note != 0) {
            ++written;
            CHECK(row[1].note != nt::engine::kNoteOff);
        }
    }
    CHECK(written == 2);
}

TEST_CASE("live loopback note records into the playing pattern", "[midi_record]") {
    nt::midi::MidiInput input;
    std::string error;
    if (!input.open_virtual_port("nt-record-loop", error)) {
        WARN("no MIDI backend available: " << error);
        return; // headless CI without ALSA seq — recorded, not failed
    }
    nt::midi::MidiOutputPort output;
    const int port = find_port(output.port_names(), "nt-record-loop");
    REQUIRE(port >= 0);
    REQUIRE(output.open_port(static_cast<unsigned>(port), error));

    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::app::ProjectSession session(audio);

    FakeCursor cursor;
    cursor.slot = 1;
    nt::app::MidiRecord record(audio, cursor);
    record.set_mode(nt::app::MidiInputMode::kRecord);
    record.set_armed_channel(2);
    record.set_velocity_to_volume(true);

    session.play();
    for (int tries = 0; tries < 100 && !audio.snapshot().transport_playing; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(audio.snapshot().transport_playing);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int row_before = audio.snapshot().row;
    const std::array<std::uint8_t, 3> note_on = {0x90, 60, 100};
    output.send(note_on.data(), note_on.size());

    // Pump the device drain exactly as ui/midi_view does per frame.
    int found_row = -1;
    for (int tries = 0; tries < 200 && found_row < 0; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        nt::midi::MidiEvent event;
        while (input.poll(event)) {
            if (event.type == nt::midi::MidiEvent::Type::kNoteOn) {
                record.on_device_note_on(session, event.data1, event.data2);
            }
        }
        const auto& rows = session.project().patterns[0].rows;
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            if (rows[static_cast<std::size_t>(r)][2].note != 0) {
                found_row = r;
                break;
            }
        }
    }
    const int row_after = audio.snapshot().row;
    session.stop();
    audio.stop();

    REQUIRE(found_row >= 0);
    const nt::engine::TrackerCell& cell =
        session.project().patterns[0].rows[static_cast<std::size_t>(found_row)][2];
    CHECK(cell.note == 49); // MIDI 60 → C-4
    CHECK(cell.instrument == 1);
    CHECK(cell.volume == 50);
    // The write lands at the playhead (round-up may lead by one row;
    // the first pattern is nowhere near wrapping in this window).
    CHECK(found_row >= row_before);
    CHECK(found_row <= row_after + 1);
}
