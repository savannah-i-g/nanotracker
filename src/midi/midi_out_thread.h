// midi/midi_out_thread — the MIDI output scheduler. A dedicated thread
// phase-locks against audio sample time (the audio thread publishes
// block-start sample count + host time in its snapshot; this thread
// interpolates between snapshots) and dispatches at ~1ms resolution,
// independent of audio block boundaries. Block-quantised sends would
// jitter by ~2.7ms at 48kHz — audible slop on hardware sequencers, and
// exactly the failure the web's lookahead scheduler existed to dodge
// (11-midi.md).
//
// Outputs: 24 PPQN MIDI clock derived from the transport (start /
// stop / song position on transport edges), plus timestamped events
// from the Ext MIDI Out graph endpoint (drained from the engine's
// ring, held until the PLL estimate reaches their at_sample, then
// sent in timestamp order) and direct queue() messages (at_sample 0 =
// send on the next pass).
#pragma once

#include "audio/audio_engine.h"
#include "midi/midi_io.h"
#include "rt/spsc_queue.h"

#include <array>
#include <atomic>
#include <thread>

namespace nt::midi {

// A raw outbound message with a target sample time (0 = immediately).
struct OutMessage {
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;
    std::uint64_t at_sample = 0;
};

class MidiOutThread {
public:
    MidiOutThread(audio::AudioEngine& engine, MidiOutputPort& port);
    ~MidiOutThread();

    MidiOutThread(const MidiOutThread&) = delete;
    MidiOutThread& operator=(const MidiOutThread&) = delete;
    MidiOutThread(MidiOutThread&&) = delete;
    MidiOutThread& operator=(MidiOutThread&&) = delete;

    void set_clock_enabled(bool enabled) { clock_enabled_ = enabled; }

    [[nodiscard]] bool clock_enabled() const { return clock_enabled_; }

    // Queue an event (any thread; audio thread uses this for graph
    // Ext MIDI Out deliveries).
    void queue(const OutMessage& message) { queue_.push(message); }

    // Telemetry for verification: clock ticks sent since start.
    [[nodiscard]] std::uint64_t clock_ticks_sent() const {
        return ticks_sent_.load(std::memory_order_relaxed);
    }

private:
    void run();

    audio::AudioEngine& engine_;
    MidiOutputPort& port_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> clock_enabled_{false};
    std::atomic<std::uint64_t> ticks_sent_{0};
    rt::SpscQueue<OutMessage> queue_{512};
    // Timestamped events awaiting their at_sample (graph deliveries
    // land here; overflow drops the newest — bounded, like every list
    // in the midi transport).
    static constexpr int kMaxPending = 256;
    std::array<OutMessage, kMaxPending> pending_{};
    int pending_count_ = 0;
    std::thread thread_;
};

} // namespace nt::midi
