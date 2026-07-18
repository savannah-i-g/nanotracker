#include "io/import/import_common.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace nt::io::import {

std::string read_string(const std::uint8_t* data, std::size_t len) {
    std::size_t end = 0;
    while (end < len && data[end] != 0) {
        ++end;
    }
    while (end > 0 && data[end - 1] == 0x20) {
        --end;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(data), end};
}

int period_to_note(int period) {
    if (period == 0) {
        return 0;
    }
    int best = 1;
    int best_diff = INT_MAX;
    for (int n = 1; n <= engine::kMaxNote; ++n) {
        const int d = std::abs(engine::note_to_period(n) - period);
        if (d < best_diff) {
            best_diff = d;
            best = n;
        }
        if (d == 0) {
            break;
        }
    }
    return best;
}

int clamp_channels(int n) {
    return std::clamp(n, 1, engine::kMaxChannels);
}

namespace {

void write_wav_header(std::vector<std::uint8_t>& out, std::uint32_t data_bytes,
                      std::uint32_t sample_rate) {
    const auto u32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFFU);
        out.push_back((v >> 8U) & 0xFFU);
        out.push_back((v >> 16U) & 0xFFU);
        out.push_back((v >> 24U) & 0xFFU);
    };
    const auto u16 = [&](std::uint16_t v) {
        out.push_back(v & 0xFFU);
        out.push_back((v >> 8U) & 0xFFU);
    };
    const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };

    tag("RIFF");
    u32(36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(sample_rate);
    u32(sample_rate * 2); // byte rate (16-bit mono)
    u16(2);               // block align
    u16(16);              // bits
    tag("data");
    u32(data_bytes);
}

} // namespace

std::vector<std::uint8_t> build_wav16(const std::vector<std::int16_t>& pcm,
                                      std::uint32_t sample_rate) {
    std::vector<std::uint8_t> out;
    out.reserve(44 + (pcm.size() * 2));
    write_wav_header(out, static_cast<std::uint32_t>(pcm.size() * 2), sample_rate);
    for (const std::int16_t s : pcm) {
        out.push_back(static_cast<std::uint8_t>(s & 0xFF));
        out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
    }
    return out;
}

std::vector<std::uint8_t> build_wav_from_float(const std::vector<float>& pcm,
                                               std::uint32_t sample_rate) {
    std::vector<std::int16_t> ints;
    ints.reserve(pcm.size());
    for (const float f : pcm) {
        const float clamped = std::clamp(f, -1.0F, 1.0F);
        ints.push_back(static_cast<std::int16_t>(clamped * 32767.0F));
    }
    return build_wav16(ints, sample_rate);
}

ModWav build_wav_mod(const std::int8_t* pcm, std::size_t length) {
    const double ratio = static_cast<double>(kModTargetRate) / kModSampleRate;
    const auto out_frames =
        static_cast<std::uint32_t>(std::ceil(static_cast<double>(length) * ratio));

    std::vector<std::int16_t> ints;
    ints.reserve(out_frames);
    const double src_ratio = static_cast<double>(kModSampleRate) / kModTargetRate;
    for (std::uint32_t i = 0; i < out_frames; ++i) {
        const double src_pos = i * src_ratio;
        const auto idx = static_cast<std::size_t>(src_pos);
        const double frac = src_pos - static_cast<double>(idx);
        const int s0 = idx < length ? pcm[idx] : 0;
        const int s1 = idx + 1 < length ? pcm[idx + 1] : s0;
        ints.push_back(
            static_cast<std::int16_t>((static_cast<double>(s0) + ((s1 - s0) * frac)) * 256.0));
    }
    return {.wav = build_wav16(ints, kModTargetRate), .out_frames = out_frames};
}

} // namespace nt::io::import
