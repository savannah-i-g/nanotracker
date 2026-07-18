#include "app/midi_record.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace nt::app {

namespace {

// MIDI note → tracker note (1..96, C-4 = 49 = MIDI 60), the web's
// shiftedMidi - 12 + 1 with its clamp (octave offset fixed at the web
// default of 0).
int tracker_note(int midi_note) {
    return std::clamp(midi_note - 11, 1, engine::kMaxNote);
}

// Velocity → volume column, web math: round((velocity/127)*64).
int velocity_volume(int velocity) {
    return static_cast<int>(std::lround(static_cast<double>(velocity) / 127.0 * 64.0));
}

} // namespace

void enter_note_cell(ProjectSession& session, int pattern_index, int row, int channel, int note,
                     int entry_slot, int volume, bool audition) {
    const engine::TrackerProject& project = session.project();
    if (pattern_index < 0 || pattern_index >= static_cast<int>(project.patterns.size())) {
        return;
    }
    const engine::TrackerPattern& pattern =
        project.patterns[static_cast<std::size_t>(pattern_index)];
    if (row < 0 || row >= static_cast<int>(pattern.rows.size()) || channel < 0 ||
        channel >= project.channels) {
        return;
    }

    // Track-bind convention (web TrackerCanvas): bound channels leave
    // instrument 0 and resolve at playback.
    std::array<int, engine::kMaxSamples> bound{};
    const int bound_count =
        engine::track_bound_instruments(project, channel, bound.data(), engine::kMaxSamples);
    const int instrument = bound_count > 0 ? 0 : entry_slot;

    engine::TrackerCell cell =
        pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(channel)];
    cell.note = static_cast<std::uint8_t>(std::clamp(note, 1, engine::kMaxNote));
    cell.instrument = static_cast<std::uint8_t>(instrument);
    if (volume >= 0) {
        cell.volume = static_cast<std::uint8_t>(std::min(volume, 0x40));
    }
    session.set_cell(pattern_index, row, channel, cell);
    if (audition) {
        const int preview_slot = bound_count > 0 ? bound[0] : entry_slot;
        session.preview_note(channel, preview_slot, cell.note);
    }
}

int quantise_record_row(const RecordAnchor& anchor, std::uint64_t at_frame) {
    if (anchor.pattern_rows <= 0) {
        return 0;
    }
    if (anchor.speed <= 0 || anchor.bpm <= 0 || anchor.sample_rate == 0) {
        return std::clamp(anchor.row, 0, anchor.pattern_rows - 1);
    }
    const double frames_per_tick =
        static_cast<double>(anchor.sample_rate) * 2.5 / static_cast<double>(anchor.bpm);
    const double row_frames = frames_per_tick * anchor.speed;
    // Signed distance from the publish instant; events stamped inside
    // the publishing pull sit at or before it, so deltas are normally
    // negative. The subtraction wraps safely for any |delta| < 2^63.
    const auto delta =
        static_cast<double>(static_cast<std::int64_t>(at_frame - anchor.stream_frames));
    const double pos = anchor.row + (((anchor.tick * frames_per_tick) + delta) / row_frames);
    // Nearest row, halves up (the header's round-up rule); wrap keeps
    // negative targets correct too.
    const auto target = static_cast<std::int64_t>(std::floor(pos + 0.5));
    const auto rows = static_cast<std::int64_t>(anchor.pattern_rows);
    return static_cast<int>(((target % rows) + rows) % rows);
}

MidiRecord::MidiRecord(audio::AudioEngine& audio, PatternCursor& cursor)
    : audio_(audio), cursor_(cursor) {}

void MidiRecord::on_device_note_on(ProjectSession& session, int midi_note, int velocity) {
    apply_note_on(session, midi_note, velocity, audio_.snapshot().stream_frames);
}

void MidiRecord::on_device_note_off(ProjectSession& session, int midi_note) {
    apply_note_off(session, midi_note);
}

void MidiRecord::update(ProjectSession& session) {
    audio::TimedMidiMessage timed;
    int budget = 256; // ring capacity — one frame always clears it
    while (budget-- > 0 && audio_.poll_bus_midi_in(timed)) {
        switch (timed.message.type) {
        case audio::MidiMessage::Type::kNoteOn:
            apply_note_on(session, timed.message.data1, timed.message.data2, timed.at_frame);
            break;
        case audio::MidiMessage::Type::kNoteOff:
            apply_note_off(session, timed.message.data1);
            break;
        case audio::MidiMessage::Type::kControlChange:
        case audio::MidiMessage::Type::kPitchBend:
            break; // notes only (web record semantics)
        }
    }
}

void MidiRecord::apply_note_on(ProjectSession& session, int midi_note, int velocity,
                               std::uint64_t at_frame) {
    switch (mode_) {
    case MidiInputMode::kOff:
        break;
    case MidiInputMode::kPreview:
        preview(session, midi_note, velocity);
        break;
    case MidiInputMode::kEnter:
        // The keyboard-entry path at the pattern cursor, plus the
        // velocity mapping; audition matches keyboard entry.
        enter_note_cell(session, cursor_.step_pattern(), cursor_.step_row(), cursor_.step_channel(),
                        tracker_note(midi_note), cursor_.step_entry_slot(),
                        velocity_to_volume_ ? velocity_volume(velocity) : -1, true);
        cursor_.step_advance();
        break;
    case MidiInputMode::kRecord:
        record(session, midi_note, velocity, at_frame);
        break;
    }
}

void MidiRecord::apply_note_off(ProjectSession& session, int midi_note) {
    // Preview release only — enter/record ignore note-offs (web
    // parity: onNoteOff never wrote cells).
    if (mode_ != MidiInputMode::kPreview) {
        return;
    }
    if (last_preview_note_ == midi_note) {
        session.preview_plugin_release();
        last_preview_note_ = -1;
    }
}

void MidiRecord::preview(ProjectSession& session, int midi_note, int velocity) {
    const int slot = preview_slot_;
    const auto& table = session.project().instrument_table;
    const bool is_plugin =
        slot >= 1 && slot <= static_cast<int>(table.size()) &&
        table[static_cast<std::size_t>(slot - 1)].type != engine::InstrumentSourceType::kSample;
    if (is_plugin) {
        session.preview_plugin_note(slot, midi_note, static_cast<float>(velocity) / 127.0F);
    } else {
        session.preview_note(armed_channel_, slot, tracker_note(midi_note));
    }
    last_preview_note_ = midi_note;
}

void MidiRecord::record(ProjectSession& session, int midi_note, int velocity,
                        std::uint64_t at_frame) {
    const audio::EngineSnapshot& snap = audio_.snapshot();
    if (!snap.transport_playing) {
        return; // web parity: record writes only while playing
    }
    const engine::TrackerProject& project = session.project();
    const int pattern_index =
        std::clamp(snap.pattern_index, 0, static_cast<int>(project.patterns.size()) - 1);
    const auto& rows = project.patterns[static_cast<std::size_t>(pattern_index)].rows;
    const RecordAnchor anchor{.row = snap.row,
                              .tick = snap.tick,
                              .speed = snap.speed,
                              .bpm = snap.bpm,
                              .pattern_rows = static_cast<int>(rows.size()),
                              .sample_rate = snap.sample_rate,
                              .stream_frames = snap.stream_frames};
    const int row = quantise_record_row(anchor, at_frame);
    const int channel = std::clamp(armed_channel_, 0, project.channels - 1);
    // No audition: the transport is already sounding the pattern.
    enter_note_cell(session, pattern_index, row, channel, tracker_note(midi_note),
                    cursor_.step_entry_slot(), velocity_to_volume_ ? velocity_volume(velocity) : -1,
                    false);
}

} // namespace nt::app
