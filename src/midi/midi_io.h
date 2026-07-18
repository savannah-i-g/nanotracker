// midi/midi_io — RtMidi device management and input. Behavioural
// reference: the web's WebMIDI layer (trackerMidi.ts, midiBus.ts);
// representation is native. Input runs on RtMidi's own callback thread
// and lands in a lock-free ring the UI thread drains once per frame —
// note entry, MIDI-learn and parameter mappings all consume from
// there. Output lives in midi_out_thread (PLL against audio time).
#pragma once

#include "rt/spsc_queue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nt::midi {

// One parsed inbound MIDI message (the byte triplet, pre-classified).
struct MidiEvent {
    enum class Type : std::uint8_t { kNoteOn, kNoteOff, kControlChange, kOther };
    Type type = Type::kOther;
    std::uint8_t channel = 0;
    std::uint8_t data1 = 0; // note / controller number
    std::uint8_t data2 = 0; // velocity / value
};

class MidiInput {
public:
    MidiInput();
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;
    MidiInput(MidiInput&&) = delete;
    MidiInput& operator=(MidiInput&&) = delete;

    [[nodiscard]] std::vector<std::string> port_names() const;

    // Opens a hardware port by index, or a virtual port other software
    // can connect to (loopback verification uses this).
    bool open_port(unsigned index, std::string& error);
    bool open_virtual_port(const std::string& name, std::string& error);
    void close_port();

    [[nodiscard]] bool is_open() const { return open_; }

    // UI thread: drain pending events (bounded per call).
    bool poll(MidiEvent& out);

private:
    static void rtmidi_callback(double delta, std::vector<unsigned char>* message, void* user_data);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    rt::SpscQueue<MidiEvent> events_{512};
    bool open_ = false;
};

class MidiOutputPort {
public:
    MidiOutputPort();
    ~MidiOutputPort();

    MidiOutputPort(const MidiOutputPort&) = delete;
    MidiOutputPort& operator=(const MidiOutputPort&) = delete;
    MidiOutputPort(MidiOutputPort&&) = delete;
    MidiOutputPort& operator=(MidiOutputPort&&) = delete;

    [[nodiscard]] std::vector<std::string> port_names() const;
    bool open_port(unsigned index, std::string& error);
    bool open_virtual_port(const std::string& name, std::string& error);
    void close_port();

    [[nodiscard]] bool is_open() const { return open_; }

    // Any thread (RtMidi serialises); the PLL thread is the only
    // caller in practice.
    void send(const std::uint8_t* bytes, std::size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool open_ = false;
};

} // namespace nt::midi
