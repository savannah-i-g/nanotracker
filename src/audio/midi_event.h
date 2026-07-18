// audio/midi_event — the frame-stamped MIDI message transport carried
// by midi-kind cables (and the rings that bridge it to hardware).
//
// Transport semantics:
//   - A midi OUTPUT port owns a per-block MidiEventList, double-
//     buffered (cur/prev) like audio blocks so delayed edges — midi
//     cables that close a cycle — read the previous block. This
//     replaces the web MidiBus's hop counter (midiBus.ts maxHops):
//     a feedback patch delays one block per lap and every list is
//     bounded per block, so event storms cannot grow unbounded.
//   - Fan-out: every destination edge copies the full source list.
//   - Fan-in (merge): a midi INPUT port concatenates all inbound lists
//     in schedule-edge order, then stable-sorts by frame — equal
//     frames keep edge order, so merges are deterministic.
//   - Overflow is counted, never UB: push past capacity increments
//     `dropped` and discards the event.
//
// Everything here is POD (rings require trivially copyable) and every
// operation is allocation-free — these types live on the audio thread.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nt::audio {

// One MIDI message on a cable, stamped with its frame offset inside
// the current block. Pitch bend rides in raw MIDI 14-bit form
// (data1 = LSB7, data2 = MSB7; signed value = ((data2<<7)|data1) -
// 8192) so encoding to wire bytes is lossless.
struct MidiMessage {
    enum class Type : std::uint8_t { kNoteOn, kNoteOff, kControlChange, kPitchBend };

    std::uint32_t frame = 0;
    Type type = Type::kNoteOn;
    std::uint8_t channel = 0; // 0-15
    std::uint8_t data1 = 0;   // note / controller / bend LSB7
    std::uint8_t data2 = 0;   // velocity / value / bend MSB7
};

// Builds a pitch-bend message from the signed -8192..8191 domain the
// effect translator works in.
[[nodiscard]] inline MidiMessage make_pitch_bend(std::uint32_t frame, std::uint8_t channel,
                                                 int bend) {
    const int raw = bend + 8192; // 0..16383
    return {.frame = frame,
            .type = MidiMessage::Type::kPitchBend,
            .channel = channel,
            .data1 = static_cast<std::uint8_t>(raw & 0x7F),
            .data2 = static_cast<std::uint8_t>((raw >> 7) & 0x7F)};
}

[[nodiscard]] inline int pitch_bend_value(const MidiMessage& message) {
    return ((static_cast<int>(message.data2) << 7) | static_cast<int>(message.data1)) - 8192;
}

// Encodes a message as wire bytes; returns the byte count (always 3
// for the carried types).
inline std::uint8_t encode_midi_message(const MidiMessage& message,
                                        std::array<std::uint8_t, 3>& bytes) {
    std::uint8_t status = 0;
    switch (message.type) {
    case MidiMessage::Type::kNoteOn:
        status = 0x90;
        break;
    case MidiMessage::Type::kNoteOff:
        status = 0x80;
        break;
    case MidiMessage::Type::kControlChange:
        status = 0xB0;
        break;
    case MidiMessage::Type::kPitchBend:
        status = 0xE0;
        break;
    }
    bytes[0] = static_cast<std::uint8_t>(status | (message.channel & 0x0F));
    bytes[1] = static_cast<std::uint8_t>(message.data1 & 0x7F);
    bytes[2] = static_cast<std::uint8_t>(message.data2 & 0x7F);
    return 3;
}

inline constexpr int kMaxMidiEventsPerBlock = 64;

// Bounded per-block event list; the midi analogue of GateEventList.
// Producers append in frame order where they can; consumers that merge
// multiple lists re-sort (sort_by_frame below).
struct MidiEventList {
    std::array<MidiMessage, kMaxMidiEventsPerBlock> events{};
    int count = 0;
    std::uint32_t dropped = 0; // events discarded because the list was full

    void clear() {
        count = 0;
        dropped = 0;
    }

    void push(const MidiMessage& message) {
        if (count < kMaxMidiEventsPerBlock) {
            events[static_cast<std::size_t>(count)] = message;
            ++count;
        } else {
            ++dropped;
        }
    }
};

// Stable insertion sort by frame — inputs are concatenations of
// already-sorted lists, so this is near-linear; equal frames keep
// insertion (= schedule-edge) order. No allocation.
inline void sort_by_frame(MidiEventList& list) {
    for (int i = 1; i < list.count; ++i) {
        const MidiMessage message = list.events[static_cast<std::size_t>(i)];
        auto j = static_cast<std::size_t>(i);
        while (j > 0 && list.events[j - 1].frame > message.frame) {
            list.events[j] = list.events[j - 1];
            --j;
        }
        list.events[j] = message;
    }
}

// A cable event annotated with its absolute stream-frame time
// (block start + frame). The master.midi.in → record/step-entry hook:
// the runner produces these into an engine-owned ring; the UI half
// drains them via AudioEngine::poll_bus_midi_in.
struct TimedMidiMessage {
    MidiMessage message;
    std::uint64_t at_frame = 0;
};

// One Ext MIDI Out delivery: raw wire bytes plus the absolute stream-
// sample time the event belongs to. The MIDI out thread holds each
// until its PLL estimate of the playback position reaches at_sample
// (0 = send immediately).
struct ExtMidiOutMessage {
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;
    std::uint64_t at_sample = 0;
};

} // namespace nt::audio
