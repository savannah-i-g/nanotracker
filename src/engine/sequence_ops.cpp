#include "engine/sequence_ops.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace nt::engine {

namespace {

constexpr int kMaxPitch = 127;
constexpr int kMaxVelocity = 127;

// Uniform [0, 1) from the top 24 bits of one mt19937 draw. mt19937's
// raw output sequence is fully specified by the standard, unlike the
// std distributions, so seeded results reproduce across platforms.
double unit_random(std::mt19937& rng) {
    return static_cast<double>(rng() >> 8U) * (1.0 / 16777216.0);
}

// Jitter in [-amount, amount], mirroring the web's
// round((random - 0.5) * 2 * amount).
long jitter(std::mt19937& rng, double amount) {
    return std::lround((unit_random(rng) - 0.5) * 2.0 * amount);
}

int clamp_pitch(int pitch) {
    return std::clamp(pitch, 0, kMaxPitch);
}

// Indices of `notes` in time order; stable, so chords keep input order.
std::vector<std::size_t> time_order(const std::vector<SequenceNote>& notes) {
    std::vector<std::size_t> order(notes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&notes](std::size_t a, std::size_t b) {
        return notes[a].start_tick < notes[b].start_tick;
    });
    return order;
}

struct TimeSpan {
    int begin = 0; // earliest start
    int end = 0;   // latest note end
};

TimeSpan span_of(const std::vector<SequenceNote>& notes) {
    TimeSpan span{notes.front().start_tick, notes.front().start_tick};
    for (const SequenceNote& note : notes) {
        span.begin = std::min(span.begin, note.start_tick);
        span.end = std::max(span.end, note.start_tick + note.duration_ticks);
    }
    return span;
}

} // namespace

std::vector<SequenceNote> quantize(const std::vector<SequenceNote>& notes, int speed, int division,
                                   int strength_percent, bool quantize_duration) {
    const int grid = std::max(1, speed / std::max(1, division));
    const double s = std::clamp(strength_percent, 0, 100) / 100.0;
    std::vector<SequenceNote> out = notes;
    for (SequenceNote& note : out) {
        const long nearest_start = std::lround(static_cast<double>(note.start_tick) / grid) * grid;
        note.start_tick = static_cast<int>(
            std::max(0L, std::lround(note.start_tick +
                                     static_cast<double>(nearest_start - note.start_tick) * s)));
        if (quantize_duration) {
            const long nearest_dur = std::max<long>(
                grid, std::lround(static_cast<double>(note.duration_ticks) / grid) * grid);
            note.duration_ticks = static_cast<int>(std::max(
                1L, std::lround(note.duration_ticks +
                                static_cast<double>(nearest_dur - note.duration_ticks) * s)));
        }
    }
    return out;
}

std::vector<SequenceNote> humanize(const std::vector<SequenceNote>& notes, int speed,
                                   std::mt19937& rng, int timing_percent, int velocity_amount,
                                   int duration_percent) {
    const int timing_ticks = speed * timing_percent / 100;
    std::vector<SequenceNote> out = notes;
    for (SequenceNote& note : out) {
        const long t = jitter(rng, timing_ticks);
        const long v = jitter(rng, velocity_amount);
        const long d = jitter(rng, note.duration_ticks * duration_percent / 100.0);
        note.start_tick = static_cast<int>(std::max(0L, note.start_tick + t));
        note.velocity = std::clamp(static_cast<int>(note.velocity + v), 1, kMaxVelocity);
        note.duration_ticks = static_cast<int>(std::max(1L, note.duration_ticks + d));
    }
    return out;
}

std::vector<SequenceNote> transpose(const std::vector<SequenceNote>& notes, int semitones) {
    std::vector<SequenceNote> out = notes;
    for (SequenceNote& note : out) {
        note.pitch = clamp_pitch(note.pitch + semitones);
    }
    return out;
}

std::vector<SequenceNote> reverse(const std::vector<SequenceNote>& notes) {
    if (notes.size() <= 1) {
        return notes;
    }
    const TimeSpan span = span_of(notes);
    std::vector<SequenceNote> out = notes;
    for (SequenceNote& note : out) {
        note.start_tick = span.end - (note.start_tick - span.begin) - note.duration_ticks;
    }
    return out;
}

std::vector<SequenceNote> invert(const std::vector<SequenceNote>& notes) {
    if (notes.size() <= 1) {
        return notes;
    }
    int lo = notes.front().pitch;
    int hi = notes.front().pitch;
    for (const SequenceNote& note : notes) {
        lo = std::min(lo, note.pitch);
        hi = std::max(hi, note.pitch);
    }
    const int center = static_cast<int>(std::lround((lo + hi) / 2.0));
    std::vector<SequenceNote> out = notes;
    for (SequenceNote& note : out) {
        note.pitch = clamp_pitch(2 * center - note.pitch);
    }
    return out;
}

std::vector<SequenceNote> arpeggiate(const std::vector<SequenceNote>& notes, int speed,
                                     ArpDirection direction, int rate, std::mt19937& rng,
                                     int gate_percent, int octave_range) {
    if (notes.empty()) {
        return {};
    }
    // Unique pitches, ascending, fanned out across the octave range.
    std::vector<int> pitches;
    pitches.reserve(notes.size());
    for (const SequenceNote& note : notes) {
        pitches.push_back(note.pitch);
    }
    std::sort(pitches.begin(), pitches.end());
    pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
    std::vector<int> expanded;
    for (int oct = 0; oct < std::max(1, octave_range); ++oct) {
        for (const int pitch : pitches) {
            const int p = pitch + oct * 12;
            if (p <= kMaxPitch) {
                expanded.push_back(p);
            }
        }
    }
    if (expanded.empty()) {
        return notes;
    }

    std::vector<int> cycle;
    switch (direction) {
    case ArpDirection::kUp:
        cycle = expanded;
        break;
    case ArpDirection::kDown:
        cycle.assign(expanded.rbegin(), expanded.rend());
        break;
    case ArpDirection::kUpDown:
        // Up then back down, dropping both turnaround duplicates.
        cycle = expanded;
        for (std::size_t i = expanded.size() - 1; i-- > 1;) {
            cycle.push_back(expanded[i]);
        }
        break;
    case ArpDirection::kRandom:
        // One Fisher–Yates shuffle; the cycle then repeats verbatim.
        cycle = expanded;
        for (std::size_t i = cycle.size() - 1; i > 0; --i) {
            const std::size_t j = rng() % (i + 1);
            std::swap(cycle[i], cycle[j]);
        }
        break;
    }

    const TimeSpan span = span_of(notes);
    const int step_ticks = std::max(1, speed / std::max(1, rate));
    const int gate_ticks = std::max(1, step_ticks * gate_percent / 100);
    long velocity_sum = 0;
    for (const SequenceNote& note : notes) {
        velocity_sum += note.velocity;
    }
    const int velocity = static_cast<int>(
        std::lround(static_cast<double>(velocity_sum) / static_cast<double>(notes.size())));

    std::vector<SequenceNote> out;
    std::size_t idx = 0;
    for (int tick = span.begin; tick < span.end; tick += step_ticks, ++idx) {
        out.push_back({.pitch = cycle[idx % cycle.size()],
                       .start_tick = tick,
                       .duration_ticks = std::min(gate_ticks, span.end - tick),
                       .velocity = velocity});
    }
    return out;
}

std::vector<SequenceNote> velocity_curve(const std::vector<SequenceNote>& notes,
                                         VelocityCurveShape shape, int min_velocity,
                                         int max_velocity) {
    if (notes.empty()) {
        return {};
    }
    const std::vector<std::size_t> order = time_order(notes);
    std::vector<SequenceNote> out = notes;
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const double t = order.size() > 1
                             ? static_cast<double>(rank) / static_cast<double>(order.size() - 1)
                             : 0.5;
        int velocity = 0;
        switch (shape) {
        case VelocityCurveShape::kCrescendo:
            velocity =
                static_cast<int>(std::lround(min_velocity + t * (max_velocity - min_velocity)));
            break;
        case VelocityCurveShape::kDecrescendo:
            velocity =
                static_cast<int>(std::lround(max_velocity - t * (max_velocity - min_velocity)));
            break;
        case VelocityCurveShape::kAccent:
            velocity = rank % 4 == 0 ? max_velocity : min_velocity;
            break;
        case VelocityCurveShape::kFlat:
            velocity = static_cast<int>(std::lround((min_velocity + max_velocity) / 2.0));
            break;
        }
        out[order[rank]].velocity = std::clamp(velocity, 1, kMaxVelocity);
    }
    return out;
}

std::vector<SequenceNote> gate_length(const std::vector<SequenceNote>& notes, int scale_percent) {
    if (notes.empty()) {
        return {};
    }
    std::vector<SequenceNote> out = notes;
    const double scale = scale_percent / 100.0;
    if (scale_percent < 100) {
        for (SequenceNote& note : out) {
            note.duration_ticks =
                static_cast<int>(std::max(1L, std::lround(note.duration_ticks * scale)));
        }
        return out;
    }
    // Legato/overlap: each note reaches scale_percent of the gap to
    // the next later start, any pitch — chord-mates share one gap.
    // Notes with nothing after them keep plain length scaling.
    const std::vector<std::size_t> order = time_order(notes);
    for (std::size_t i = 0; i < order.size(); ++i) {
        SequenceNote& note = out[order[i]];
        long target = std::lround(note.duration_ticks * scale);
        for (std::size_t j = i + 1; j < order.size(); ++j) {
            const int next_start = out[order[j]].start_tick;
            if (next_start > note.start_tick) {
                target = std::lround((next_start - note.start_tick) * scale);
                break;
            }
        }
        note.duration_ticks = static_cast<int>(std::max(1L, target));
    }
    return out;
}

} // namespace nt::engine
