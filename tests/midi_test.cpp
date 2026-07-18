// MIDI verification: the effect→MIDI translator (byte-exact against
// the web mapper's math), the tracker-bus midi ports and Ext MIDI
// bridges through a deterministic offline engine, CV→plugin-param
// motion, FTRK persistence of the Ext nodes — plus the live ALSA
// loopback suite (virtual ports, WARN-skip on headless CI): raw
// note/cc delivery, the PLL clock thread, and Ext MIDI Out end to end.
#include "app/project_session.h"
#include "audio/effect_midi.h"
#include "midi/midi_io.h"
#include "midi/midi_out_thread.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <miniz.h>
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

// In-memory ZIP for NTP archives (as in ntp_test.cpp).
std::vector<std::uint8_t>
make_zip(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0) == MZ_TRUE);
    for (const auto& [name, data] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                                      MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
    }
    void* buf = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size) == MZ_TRUE);
    std::vector<std::uint8_t> bytes(static_cast<std::uint8_t*>(buf),
                                    static_cast<std::uint8_t*>(buf) + size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return bytes;
}

std::vector<std::uint8_t> to_bytes(const std::string& text) {
    return {text.begin(), text.end()};
}

// Drains every queued Ext MIDI Out delivery from the engine.
std::vector<nt::audio::ExtMidiOutMessage> drain_ext_out(nt::audio::AudioEngine& audio) {
    std::vector<nt::audio::ExtMidiOutMessage> out;
    nt::audio::ExtMidiOutMessage message{};
    while (audio.poll_ext_midi_out(message)) {
        out.push_back(message);
    }
    return out;
}

} // namespace

TEST_CASE("effect to midi ports the web mapper's math exactly", "[midi]") {
    nt::audio::EffectMidiState state; // web defaults: vol 100, pan 64, bend 0
    nt::audio::MidiEventList list;

    // Dxx family: 0xA volume slide up 4 compounds from the carrier.
    nt::audio::effect_to_midi(0xA, 0x40, 0, 5, state, list);
    REQUIRE(list.count == 1);
    CHECK(list.events[0].type == nt::audio::MidiMessage::Type::kControlChange);
    CHECK(list.events[0].frame == 5);
    CHECK(list.events[0].data1 == 7);
    CHECK(list.events[0].data2 == 104);                      // 100 + 4
    nt::audio::effect_to_midi(0xA, 0x04, 0, 5, state, list); // down 4
    REQUIRE(list.count == 2);
    CHECK(list.events[1].data2 == 100);
    // Fine slides via the extended nibbles (EAx/EBx re-encode).
    nt::audio::effect_to_midi(0xE, 0xA5, 0, 5, state, list); // fine up 5
    REQUIRE(list.count == 3);
    CHECK(list.events[2].data2 == 105);
    nt::audio::effect_to_midi(0xE, 0xB2, 0, 5, state, list); // fine down 2
    REQUIRE(list.count == 4);
    CHECK(list.events[3].data2 == 103);

    // Mxx absolute: set volume 32/64 → round(63.5) = 64.
    nt::audio::effect_to_midi(0xC, 0x20, 0, 6, state, list);
    REQUIRE(list.count == 5);
    CHECK(list.events[4].data2 == 64);
    // Unchanged value suppresses (web `next !== last`).
    nt::audio::effect_to_midi(0xC, 0x20, 0, 7, state, list);
    CHECK(list.count == 5);

    // Fxx/Exx coarse bends: value*16 from the running bend.
    nt::audio::effect_to_midi(0x1, 0x10, 0, 8, state, list); // up: 0 + 256
    REQUIRE(list.count == 6);
    CHECK(list.events[5].type == nt::audio::MidiMessage::Type::kPitchBend);
    CHECK(nt::audio::pitch_bend_value(list.events[5]) == 256);
    CHECK(list.events[5].data1 == 0x00); // (256+8192) = 0x2100
    CHECK(list.events[5].data2 == 0x42);
    nt::audio::effect_to_midi(0x2, 0x08, 0, 8, state, list); // down: 256-128
    REQUIRE(list.count == 7);
    CHECK(nt::audio::pitch_bend_value(list.events[6]) == 128);

    // Gxx signed step: (v-128)*32; centre value emits nothing.
    nt::audio::effect_to_midi(0x3, 0x80, 0, 9, state, list);
    CHECK(list.count == 7);
    nt::audio::effect_to_midi(0x3, 0x90, 0, 9, state, list); // +512
    REQUIRE(list.count == 8);
    CHECK(nt::audio::pitch_bend_value(list.events[7]) == 640);

    // Xxx pan: 0..255 → CC10 0..127 with round-half-up and clamp.
    nt::audio::effect_to_midi(0x8, 0xFF, 1, 10, state, list);
    REQUIRE(list.count == 9);
    CHECK(list.events[8].channel == 1);
    CHECK(list.events[8].data1 == 10);
    CHECK(list.events[8].data2 == 127);
    nt::audio::effect_to_midi(0x8, 0x80, 1, 10, state, list); // 64
    REQUIRE(list.count == 10);
    CHECK(list.events[9].data2 == 64);
    nt::audio::effect_to_midi(0x8, 0x80, 1, 11, state, list); // unchanged
    CHECK(list.count == 10);

    // Transport and LFO effects emit nothing (Txx = clock-period only).
    nt::audio::effect_to_midi(0xF, 0x80, 0, 12, state, list); // tempo 128
    nt::audio::effect_to_midi(0x4, 0x53, 0, 12, state, list); // vibrato
    nt::audio::effect_to_midi(0xB, 0x01, 0, 12, state, list); // jump
    CHECK(list.count == 10);
}

TEST_CASE("offline engine streams row events through an Ext MIDI Out node", "[midi]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);

    const std::string emo = session.add_ext_midi_out_node();
    REQUIRE_FALSE(emo.empty());
    REQUIRE(session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch01.midi"},
                              {.node_id = emo, .port_id = "midi"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);

    // Scripted pattern: note, Mxx, Dxx, Fxx-porta, note-off. Default
    // project: 125 BPM speed 6 → tick = 960 frames, row = 5760.
    nt::engine::TrackerCell cell;
    cell.note = 49; // C-4 → MIDI 60
    cell.instrument = 1;
    session.set_cell(0, 0, 0, cell);
    cell = {};
    cell.effect = 0xC; // set volume 32 → CC7 64
    cell.effect_param = 0x20;
    session.set_cell(0, 1, 0, cell);
    cell = {};
    cell.effect = 0xA; // slide up 4 → CC7 68
    cell.effect_param = 0x40;
    session.set_cell(0, 2, 0, cell);
    cell = {};
    cell.effect = 0x1; // porta up 0x10 → bend +256
    cell.effect_param = 0x10;
    session.set_cell(0, 3, 0, cell);
    cell = {};
    cell.note = nt::engine::kNoteOff;
    session.set_cell(0, 4, 0, cell);

    session.play();
    std::array<float, 256> block{};
    for (int i = 0; i < 188; ++i) { // 188 × 128 = 24064 frames > row 4
        audio.render_offline(block.data(), 128);
    }
    session.stop();

    const std::vector<nt::audio::ExtMidiOutMessage> wire = drain_ext_out(audio);
    REQUIRE(wire.size() == 5);
    // Row 0 @ 0: note on ch0, C-4, velocity 127 (volume 64).
    CHECK(wire[0].bytes[0] == 0x90);
    CHECK(wire[0].bytes[1] == 60);
    CHECK(wire[0].bytes[2] == 127);
    CHECK(wire[0].at_sample == 0);
    // Row 1 @ 5760: Mxx 32 → CC7 64.
    CHECK(wire[1].bytes[0] == 0xB0);
    CHECK(wire[1].bytes[1] == 7);
    CHECK(wire[1].bytes[2] == 64);
    CHECK(wire[1].at_sample == 5760);
    // Row 2 @ 11520: Dxx +4 → CC7 68.
    CHECK(wire[2].bytes[0] == 0xB0);
    CHECK(wire[2].bytes[2] == 68);
    CHECK(wire[2].at_sample == 11520);
    // Row 3 @ 17280: pitch bend +256 = raw 0x2100.
    CHECK(wire[3].bytes[0] == 0xE0);
    CHECK(wire[3].bytes[1] == 0x00);
    CHECK(wire[3].bytes[2] == 0x42);
    CHECK(wire[3].at_sample == 17280);
    // Row 4 @ 23040: note off.
    CHECK(wire[4].bytes[0] == 0x80);
    CHECK(wire[4].bytes[1] == 60);
    CHECK(wire[4].at_sample == 23040);
}

TEST_CASE("offline engine bridges an Ext MIDI In ring through cables", "[midi]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::rt::SpscQueue<nt::audio::MidiMessage> ring{64};
    audio.set_ext_midi_source(&ring);
    nt::app::ProjectSession session(audio);

    const std::string emi = session.add_ext_midi_in_node();
    const std::string emo = session.add_ext_midi_out_node();
    REQUIRE(session.add_cable({.node_id = emi, .port_id = "midi"},
                              {.node_id = emo, .port_id = "midi"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);

    std::array<float, 256> block{};
    audio.render_offline(block.data(), 128); // drain session publishes

    ring.push({.frame = 0,
               .type = nt::audio::MidiMessage::Type::kNoteOn,
               .channel = 3,
               .data1 = 72,
               .data2 = 99});
    audio.render_offline(block.data(), 128);

    const std::vector<nt::audio::ExtMidiOutMessage> wire = drain_ext_out(audio);
    REQUIRE(wire.size() == 1);
    CHECK(wire[0].bytes[0] == 0x93);
    CHECK(wire[0].bytes[1] == 72);
    CHECK(wire[0].bytes[2] == 99);
    // Arrival quantised to the block it was drained in (block 2 starts
    // at frame 128).
    CHECK(wire[0].at_sample == 128);
}

TEST_CASE("cv cable drives an NTP instance parameter", "[midi]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);

    // Instrument with a host param bound to amp.gain (range 0..1).
    const std::string manifest_json = R"({
      "ntp": 1, "id": "test.cvparam", "name": "CV PARAM", "type": "instrument",
      "params": [
        {"key": "amp.gain", "label": "AMP", "min": 0, "max": 1, "default": 0}
      ],
      "voices": 2,
      "graph": {
        "nodes": [
          {"id": "osc", "type": "oscillator", "scope": "voice",
           "oscType": "sine", "frequency": 220, "gain": 0.5},
          {"id": "amp", "type": "gain", "scope": "voice", "gain": 0}
        ],
        "connections": [
          {"from": "osc", "to": "amp"},
          {"from": "amp", "to": "voiceOut"}
        ]
      }
    })";
    const std::filesystem::path ntp_path =
        std::filesystem::temp_directory_path() / "nt_cv_param.ntplug";
    {
        const std::vector<std::uint8_t> zip = make_zip({{"plugin.json", to_bytes(manifest_json)}});
        std::ofstream out(ntp_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(zip.data()),
                  static_cast<std::streamsize>(zip.size()));
    }
    const std::string plugin_id = session.load_plugin_file(ntp_path);
    REQUIRE_FALSE(plugin_id.empty());
    const std::string node_id = session.add_plugin_node(plugin_id);
    REQUIRE_FALSE(node_id.empty());

    // The session generated a CV port per host param, id = dot-path.
    const nt::graph::Node* node = session.workspace().find_node(node_id);
    REQUIRE(node != nullptr);
    const nt::graph::Port* cv_port = nt::graph::find_input(*node, "amp.gain");
    REQUIRE(cv_port != nullptr);
    CHECK(cv_port->kind == nt::graph::PortKind::kCv);
    CHECK(cv_port->param_ref == 0);

    REQUIRE(session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch01.vol.cv"},
                              {.node_id = node_id, .port_id = "amp.gain"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);

    // An active channel voice puts the channel gain (1.0) on the CV
    // out; the slewed value must reach the instance parameter.
    const std::filesystem::path wav_path =
        std::filesystem::temp_directory_path() / "nt_cv_param.wav";
    {
        const std::vector<std::uint8_t> bytes = make_wav_public(500.0F, 0.5F, 48000);
        std::ofstream out(wav_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    REQUIRE(session.load_sample_into_slot(1, wav_path));
    nt::engine::TrackerCell cell;
    cell.note = 49;
    cell.instrument = 1;
    session.set_cell(0, 0, 0, cell);

    nt::plugins::NtpInstance* instance = session.plugin_instance(node_id);
    REQUIRE(instance != nullptr);
    CHECK(instance->param_value("amp.gain") == 0.0F);

    session.play();
    std::array<float, 256> block{};
    for (int i = 0; i < 60; ++i) { // ~160ms >> the 10ms slew
        audio.render_offline(block.data(), 128);
    }
    session.stop();
    CHECK(instance->param_value("amp.gain") > 0.8F);
}

TEST_CASE("ftrk persists ext midi nodes and midi cables", "[midi]") {
    nt::audio::AudioEngine audio;
    REQUIRE(audio.start_offline(48000));
    nt::app::ProjectSession session(audio);

    const std::string emi = session.add_ext_midi_in_node();
    const std::string emo = session.add_ext_midi_out_node();
    REQUIRE(session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi"},
                              {.node_id = emo, .port_id = "midi"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);
    REQUIRE(session.add_cable({.node_id = emi, .port_id = "midi"},
                              {.node_id = nt::graph::kTrackerBusId, .port_id = "master.midi.in"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "nt_ext_midi.ftrk";
    REQUIRE(session.save_ftrk(path));

    nt::app::ProjectSession loaded(audio);
    REQUIRE(loaded.load_ftrk(path));
    const nt::graph::Node* emi_node = loaded.workspace().find_node(emi);
    REQUIRE(emi_node != nullptr);
    CHECK(emi_node->kind == nt::graph::NodeKind::kExtMidiIn);
    const nt::graph::Node* emo_node = loaded.workspace().find_node(emo);
    REQUIRE(emo_node != nullptr);
    CHECK(emo_node->kind == nt::graph::NodeKind::kExtMidiOut);
    REQUIRE(loaded.workspace().cables().size() == 2);
    for (const nt::graph::Cable& cable : loaded.workspace().cables()) {
        CHECK(cable.src_kind == nt::graph::PortKind::kMidi);
        CHECK(cable.dst_kind == nt::graph::PortKind::kMidi);
    }
}

TEST_CASE("live ext midi out reaches hardware through the pll thread", "[midi]") {
    nt::midi::MidiInput loopback;
    std::string error;
    if (!loopback.open_virtual_port("nt-ext-out-loop", error)) {
        WARN("no MIDI backend available: " << error);
        return;
    }
    nt::audio::AudioEngine audio;
    if (!audio.start()) {
        WARN("no audio device: " << audio.device().error());
        return;
    }
    nt::midi::MidiOutputPort output;
    const int port = find_port(output.port_names(), "nt-ext-out-loop");
    REQUIRE(port >= 0);
    REQUIRE(output.open_port(static_cast<unsigned>(port), error));
    nt::midi::MidiOutThread out_thread(audio, output);

    nt::app::ProjectSession session(audio);
    const std::string emo = session.add_ext_midi_out_node();
    REQUIRE(session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch01.midi"},
                              {.node_id = emo, .port_id = "midi"},
                              nt::graph::CableMode::kTap) == nt::graph::ConnectResult::kOk);
    nt::engine::TrackerCell cell;
    cell.note = 49; // C-4 → MIDI 60
    cell.instrument = 1;
    session.set_cell(0, 0, 0, cell);

    session.play();
    std::vector<nt::midi::MidiEvent> received;
    for (int tries = 0; tries < 200; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        nt::midi::MidiEvent event;
        while (loopback.poll(event)) {
            received.push_back(event);
        }
        if (!received.empty()) {
            break;
        }
    }
    session.stop();
    audio.stop();

    REQUIRE_FALSE(received.empty());
    CHECK(received.front().type == nt::midi::MidiEvent::Type::kNoteOn);
    CHECK(received.front().data1 == 60);
    CHECK(received.front().data2 == 127);
}

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
