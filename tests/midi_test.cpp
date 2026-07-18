// MIDI verification over a real ALSA loopback: a virtual input port is
// created in-process, the output port connects to it, and everything
// sent is read back — note events end to end, then the PLL clock
// thread's 24 PPQN stream against the live audio transport (rate
// checked against BPM within tolerance).
#include "app/project_session.h"
#include "midi/midi_io.h"
#include "midi/midi_out_thread.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

// Minimal 16-bit mono PCM WAV (as in ntp_test.cpp).
std::vector<std::uint8_t> make_wav_public(float freq, float seconds, std::uint32_t rate) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<std::uint8_t> bytes(44 + (static_cast<std::size_t>(frames) * 2));
    auto put32 = [&bytes](std::size_t at, std::uint32_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    };
    auto put16 = [&bytes](std::size_t at, std::uint16_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36 + (frames * 2));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        const auto v = static_cast<std::int16_t>(s * 20000.0F);
        put16(44 + (static_cast<std::size_t>(i) * 2), static_cast<std::uint16_t>(v));
    }
    return bytes;
}

} // namespace

namespace {

// Finds the loopback port by name in the out's port list.
int find_port(const std::vector<std::string>& names, const std::string& needle) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i].find(needle) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

TEST_CASE("midi loopback delivers note and cc events", "[midi]") {
    nt::midi::MidiInput input;
    std::string error;
    if (!input.open_virtual_port("nt-test-loop", error)) {
        WARN("no MIDI backend available: " << error);
        return; // headless CI without ALSA seq — recorded, not failed
    }
    nt::midi::MidiOutputPort output;
    const int port = find_port(output.port_names(), "nt-test-loop");
    REQUIRE(port >= 0);
    REQUIRE(output.open_port(static_cast<unsigned>(port), error));

    const std::uint8_t note_on[3] = {0x90, 69, 100};
    const std::uint8_t cc[3] = {0xB1, 74, 42};
    const std::uint8_t note_off[3] = {0x80, 69, 0};
    output.send(note_on, 3);
    output.send(cc, 3);
    output.send(note_off, 3);

    std::vector<nt::midi::MidiEvent> received;
    for (int tries = 0; tries < 100 && received.size() < 3; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        nt::midi::MidiEvent event;
        while (input.poll(event)) {
            received.push_back(event);
        }
    }
    REQUIRE(received.size() == 3);
    CHECK(received[0].type == nt::midi::MidiEvent::Type::kNoteOn);
    CHECK(received[0].data1 == 69);
    CHECK(received[0].data2 == 100);
    CHECK(received[1].type == nt::midi::MidiEvent::Type::kControlChange);
    CHECK(received[1].channel == 1);
    CHECK(received[1].data1 == 74);
    CHECK(received[1].data2 == 42);
    CHECK(received[2].type == nt::midi::MidiEvent::Type::kNoteOff);
}

TEST_CASE("midi clock ticks at 24 ppqn against the live transport", "[midi]") {
    nt::midi::MidiInput input;
    std::string error;
    if (!input.open_virtual_port("nt-clock-loop", error)) {
        WARN("no MIDI backend available: " << error);
        return;
    }
    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::app::ProjectSession session(audio);

    nt::midi::MidiOutputPort output;
    const int port = find_port(output.port_names(), "nt-clock-loop");
    REQUIRE(port >= 0);
    REQUIRE(output.open_port(static_cast<unsigned>(port), error));

    nt::midi::MidiOutThread clock(audio, output);
    clock.set_clock_enabled(true);
    session.play(); // default project, 125 BPM

    // 125 BPM × 24 PPQN = 50 ticks/second; measure ~0.8s.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    session.stop();
    const std::uint64_t sent = clock.clock_ticks_sent();
    // Tolerant window: startup latency + snapshot cadence.
    CHECK(sent >= 25);
    CHECK(sent <= 55);
    audio.stop();
}

TEST_CASE("sequence layers render voices through the live engine", "[seq]") {
    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::app::ProjectSession session(audio);

    // Give slot 1 a real sample: generate a WAV on disk and load it.
    const std::filesystem::path wav_path =
        std::filesystem::temp_directory_path() / "nt_seq_test.wav";
    {
        // 0.3s 500Hz sine, 16-bit mono 48k.
        std::vector<std::uint8_t> bytes = make_wav_public(500.0F, 0.3F, 48000);
        std::ofstream out(wav_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(session.load_sample_into_slot(1, wav_path));

    // One long seq note; the pattern grid stays empty.
    session.seq_add_note(0, 0, 0,
                         {.pitch = 60, .start_tick = 0, .duration_ticks = 600, .velocity = 120});
    session.seq_set_layer_instrument(0, 0, 0, 1);
    session.play();
    float peak = 0.0F;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        peak = std::max(peak, audio.snapshot().peak_l);
    }
    session.stop();
    audio.stop();
    CHECK(peak > 0.05F);
}
