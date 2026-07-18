#include "midi/midi_io.h"

#include <RtMidi.h>

namespace nt::midi {

// ── MidiInput ────────────────────────────────────────────────────────

struct MidiInput::Impl {
    std::unique_ptr<RtMidiIn> in;
};

MidiInput::MidiInput() : impl_(std::make_unique<Impl>()) {
    try {
        impl_->in = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, "nanoTracker");
    } catch (const RtMidiError&) {
        impl_->in = nullptr; // no MIDI backend — poll() stays empty
    }
}

MidiInput::~MidiInput() {
    close_port();
}

void MidiInput::rtmidi_callback(double delta, std::vector<unsigned char>* message,
                                void* user_data) {
    (void)delta;
    auto* self = static_cast<MidiInput*>(user_data);
    if (message == nullptr || message->empty()) {
        return;
    }
    const std::uint8_t status = (*message)[0];
    MidiEvent event;
    event.channel = status & 0x0F;
    event.data1 = message->size() > 1 ? (*message)[1] : 0;
    event.data2 = message->size() > 2 ? (*message)[2] : 0;
    switch (status & 0xF0) {
    case 0x90:
        event.type = event.data2 > 0 ? MidiEvent::Type::kNoteOn : MidiEvent::Type::kNoteOff;
        break;
    case 0x80:
        event.type = MidiEvent::Type::kNoteOff;
        break;
    case 0xB0:
        event.type = MidiEvent::Type::kControlChange;
        break;
    default:
        event.type = MidiEvent::Type::kOther;
        break;
    }
    self->events_.push(event); // full ring drops (bounded input)

    // Graph tap: the cable-transport families only (running-status-
    // free RtMidi messages; clock/sysex never reach here — ignoreTypes
    // filters them at open).
    audio::MidiMessage graph_message;
    graph_message.channel = status & 0x0F;
    graph_message.data1 = event.data1 & 0x7F;
    graph_message.data2 = event.data2 & 0x7F;
    switch (status & 0xF0) {
    case 0x90:
        graph_message.type = event.data2 > 0 ? audio::MidiMessage::Type::kNoteOn
                                             : audio::MidiMessage::Type::kNoteOff;
        break;
    case 0x80:
        graph_message.type = audio::MidiMessage::Type::kNoteOff;
        break;
    case 0xB0:
        graph_message.type = audio::MidiMessage::Type::kControlChange;
        break;
    case 0xE0:
        graph_message.type = audio::MidiMessage::Type::kPitchBend;
        break;
    default:
        return; // not a cable-transport message
    }
    self->graph_events_.push(graph_message); // full ring drops
}

std::vector<std::string> MidiInput::port_names() const {
    std::vector<std::string> names;
    if (impl_->in == nullptr) {
        return names;
    }
    const unsigned count = impl_->in->getPortCount();
    names.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        names.push_back(impl_->in->getPortName(i));
    }
    return names;
}

bool MidiInput::open_port(unsigned index, std::string& error) {
    if (impl_->in == nullptr) {
        error = "no MIDI backend";
        return false;
    }
    try {
        impl_->in->openPort(index, "nanoTracker in");
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return false;
    }
    impl_->in->setCallback(&MidiInput::rtmidi_callback, this);
    impl_->in->ignoreTypes(true, true, true);
    open_ = true;
    return true;
}

bool MidiInput::open_virtual_port(const std::string& name, std::string& error) {
    if (impl_->in == nullptr) {
        error = "no MIDI backend";
        return false;
    }
#ifdef _WIN32
    // WinMM has no virtual ports; RtMidi reports this as a non-throwing
    // warning, so an honest refusal has to come from us.
    (void)name;
    error = "virtual MIDI ports are not supported by the WinMM backend";
    return false;
#else
    try {
        impl_->in->openVirtualPort(name);
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return false;
    }
    impl_->in->setCallback(&MidiInput::rtmidi_callback, this);
    impl_->in->ignoreTypes(true, true, true);
    open_ = true;
    return true;
#endif
}

void MidiInput::close_port() {
    if (impl_->in != nullptr && open_) {
        impl_->in->cancelCallback();
        impl_->in->closePort();
    }
    open_ = false;
}

bool MidiInput::poll(MidiEvent& out) {
    return events_.pop(out);
}

// ── MidiOutputPort ───────────────────────────────────────────────────

struct MidiOutputPort::Impl {
    std::unique_ptr<RtMidiOut> out;
};

MidiOutputPort::MidiOutputPort() : impl_(std::make_unique<Impl>()) {
    try {
        impl_->out = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, "nanoTracker");
    } catch (const RtMidiError&) {
        impl_->out = nullptr;
    }
}

MidiOutputPort::~MidiOutputPort() {
    close_port();
}

std::vector<std::string> MidiOutputPort::port_names() const {
    std::vector<std::string> names;
    if (impl_->out == nullptr) {
        return names;
    }
    const unsigned count = impl_->out->getPortCount();
    names.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        names.push_back(impl_->out->getPortName(i));
    }
    return names;
}

bool MidiOutputPort::open_port(unsigned index, std::string& error) {
    if (impl_->out == nullptr) {
        error = "no MIDI backend";
        return false;
    }
    try {
        impl_->out->openPort(index, "nanoTracker out");
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return false;
    }
    open_ = true;
    return true;
}

bool MidiOutputPort::open_virtual_port(const std::string& name, std::string& error) {
    if (impl_->out == nullptr) {
        error = "no MIDI backend";
        return false;
    }
#ifdef _WIN32
    // Same WinMM limitation as MidiInput::open_virtual_port.
    (void)name;
    error = "virtual MIDI ports are not supported by the WinMM backend";
    return false;
#else
    try {
        impl_->out->openVirtualPort(name);
    } catch (const RtMidiError& e) {
        error = e.getMessage();
        return false;
    }
    open_ = true;
    return true;
#endif
}

void MidiOutputPort::close_port() {
    if (impl_->out != nullptr && open_) {
        impl_->out->closePort();
    }
    open_ = false;
}

void MidiOutputPort::send(const std::uint8_t* bytes, std::size_t size) {
    if (impl_->out != nullptr && open_) {
        try {
            impl_->out->sendMessage(bytes, size);
        } catch (const RtMidiError&) { // NOLINT(bugprone-empty-catch)
            // Device unplugged mid-send — the port stays "open" until
            // the user re-picks; sends are best-effort.
        }
    }
}

} // namespace nt::midi
