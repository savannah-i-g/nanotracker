#include "midi/midi_out_thread.h"

#include <array>
#include <chrono>

namespace nt::midi {

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

MidiOutThread::MidiOutThread(audio::AudioEngine& engine, MidiOutputPort& port)
    : engine_(engine), port_(port), thread_([this] { run(); }) {}

MidiOutThread::~MidiOutThread() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MidiOutThread::run() {
    bool was_playing = false;
    double next_tick_sample = 0.0;

    while (!stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const audio::EngineSnapshot& snap = engine_.snapshot();
        if (snap.sample_rate == 0) {
            continue;
        }
        // PLL: estimate the device's current sample position from the
        // last published (block start sample, host time) pair.
        const double host_now = now_seconds();
        const double estimated_sample = static_cast<double>(snap.stream_frames) +
                                        ((host_now - snap.host_time_seconds) * snap.sample_rate);

        // Collect new work into the pending set: direct queue()
        // messages (at_sample 0 = immediate) and Ext MIDI Out graph
        // deliveries from the engine ring (stream-sample timestamps).
        auto hold = [this](const OutMessage& held) {
            if (pending_count_ < kMaxPending) {
                pending_[static_cast<std::size_t>(pending_count_)] = held;
                ++pending_count_;
            } // full = dropped; bounded like the rest of the transport
        };
        OutMessage message{};
        while (queue_.pop(message)) {
            if (message.at_sample == 0) {
                port_.send(message.bytes.data(), message.size);
            } else {
                hold(message);
            }
        }
        audio::ExtMidiOutMessage ext{};
        while (engine_.poll_ext_midi_out(ext)) {
            hold({.bytes = ext.bytes, .size = ext.size, .at_sample = ext.at_sample});
        }

        // Dispatch everything due, in timestamp order (repeated min
        // scan — pending is small and the cadence is 1ms). Events are
        // stamped at render time, which runs ahead of the estimate by
        // the device latency: holding until the estimate catches up is
        // what phase-aligns cable events with the audible output.
        for (;;) {
            int due = -1;
            for (int i = 0; i < pending_count_; ++i) {
                const OutMessage& candidate = pending_[static_cast<std::size_t>(i)];
                if (static_cast<double>(candidate.at_sample) <= estimated_sample &&
                    (due < 0 ||
                     candidate.at_sample < pending_[static_cast<std::size_t>(due)].at_sample)) {
                    due = i;
                }
            }
            if (due < 0) {
                break;
            }
            port_.send(pending_[static_cast<std::size_t>(due)].bytes.data(),
                       pending_[static_cast<std::size_t>(due)].size);
            pending_[static_cast<std::size_t>(due)] =
                pending_[static_cast<std::size_t>(pending_count_ - 1)];
            --pending_count_;
        }

        if (!clock_enabled_.load(std::memory_order_relaxed)) {
            was_playing = false;
            continue;
        }

        // Transport edges → start/stop (position 0 start; the tracker
        // transport always starts from the pattern top natively).
        if (snap.transport_playing && !was_playing) {
            const std::array<std::uint8_t, 3> song_position = {0xF2, 0, 0};
            port_.send(song_position.data(), 3);
            const std::uint8_t start = 0xFA;
            port_.send(&start, 1);
            next_tick_sample = estimated_sample;
        } else if (!snap.transport_playing && was_playing) {
            const std::uint8_t stop_byte = 0xFC;
            port_.send(&stop_byte, 1);
        }
        was_playing = snap.transport_playing;
        if (!snap.transport_playing || snap.bpm <= 0) {
            continue;
        }

        // 24 PPQN: tick period in samples. A tracker row at speed 6 is
        // a 16th note → quarter = 4 rows = 24 ticks; period derives
        // from BPM alone, exactly like the web ppq24 counter.
        const double tick_period =
            static_cast<double>(snap.sample_rate) * 60.0 / (static_cast<double>(snap.bpm) * 24.0);
        while (next_tick_sample <= estimated_sample) {
            const std::uint8_t clock = 0xF8;
            port_.send(&clock, 1);
            ticks_sent_.fetch_add(1, std::memory_order_relaxed);
            next_tick_sample += tick_period;
        }
    }
}

} // namespace nt::midi
