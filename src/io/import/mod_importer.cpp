#include "io/import/mod_importer.h"

#include "engine/tracker_engine.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace nt::io::import {

namespace {

struct Format {
    int channels = 4;
    bool is31 = false;
};

Format detect_format(const std::uint8_t* data, std::size_t size) {
    if (size < 1084) {
        return {.channels = 4, .is31 = false}; // 15-sample SoundTracker
    }
    const std::string tag(reinterpret_cast<const char*>(data + 1080),
                          4); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

    static constexpr std::array<const char*, 8> kFourChannel = {"M.K.", "M!K!", "FLT4", "4CHN",
                                                                "4FLT", "N.T.", "M&K!", "FEST"};
    for (const char* t : kFourChannel) {
        if (tag == t) {
            return {.channels = 4, .is31 = true};
        }
    }

    struct TagChannels {
        const char* tag;
        int channels;
    };

    static constexpr std::array<TagChannels, 6> kTagChannels = {
        {{"FLT8", 8}, {"8CHN", 8}, {"OCTA", 8}, {"CD81", 8}, {"6CHN", 6}, {"2CHN", 2}}};
    for (const TagChannels& t : kTagChannels) {
        if (tag == t.tag) {
            return {.channels = t.channels, .is31 = true};
        }
    }
    // "xCHN" and "xxCH" arbitrary channel counts.
    if (std::isdigit(tag[0]) != 0 && tag.substr(1) == "CHN") {
        return {.channels = tag[0] - '0', .is31 = true};
    }
    if (std::isdigit(tag[0]) != 0 && std::isdigit(tag[1]) != 0 && tag.substr(2) == "CH") {
        return {.channels = ((tag[0] - '0') * 10) + (tag[1] - '0'), .is31 = true};
    }
    return {.channels = 4, .is31 = false};
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

std::optional<engine::TrackerProject> import_mod(const std::uint8_t* data, std::size_t size,
                                                 ImportResults& results) {
    if (size < 20 + (15 * 30) + 130) {
        results.errors.emplace_back("file too small to be a MOD");
        return std::nullopt;
    }

    const Format format = detect_format(data, size);
    const int raw_channels = format.channels;
    const int num_sample_slots = format.is31 ? 31 : 15;
    const int channels = clamp_channels(raw_channels);
    if (!format.is31) {
        results.info.emplace_back("Detected 15-sample SoundTracker format");
    }

    engine::TrackerProject project;
    project.name = upper(read_string(data, 20));
    if (project.name.empty()) {
        project.name = "UNTITLED";
    }
    project.name.resize(std::min<std::size_t>(project.name.size(), 32));
    project.channels = channels;
    project.rows_per_pattern = 64;

    struct RawSample {
        std::string name;
        std::uint32_t length_bytes = 0;
        int finetune = 0;
        int volume = 64;
        std::uint32_t loop_start_bytes = 0;
        std::uint32_t loop_length_bytes = 0;
    };

    std::vector<RawSample> raw_samples;
    std::size_t offset = 20;
    for (int i = 0; i < num_sample_slots; ++i) {
        RawSample rs;
        rs.name = read_string(data + offset, 22);
        rs.length_bytes =
            (static_cast<std::uint32_t>(data[offset + 22]) << 8U | data[offset + 23]) * 2;
        const int fin_raw = data[offset + 24] & 0x0F;
        rs.finetune = fin_raw >= 8 ? fin_raw - 16 : fin_raw;
        rs.volume = std::min<int>(64, data[offset + 25]);
        rs.loop_start_bytes =
            (static_cast<std::uint32_t>(data[offset + 26]) << 8U | data[offset + 27]) * 2;
        rs.loop_length_bytes =
            (static_cast<std::uint32_t>(data[offset + 28]) << 8U | data[offset + 29]) * 2;
        raw_samples.push_back(std::move(rs));
        offset += 30;
    }

    const int song_length = data[offset];
    offset += 2; // song length + restart byte
    std::array<int, 128> order_table{};
    int max_pattern = 0;
    for (int i = 0; i < 128; ++i) {
        order_table[static_cast<std::size_t>(i)] = data[offset++];
        max_pattern = std::max(max_pattern, order_table[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i < std::max(1, song_length); ++i) {
        project.order_list.push_back(order_table[static_cast<std::size_t>(i)]);
    }
    if (format.is31) {
        offset += 4; // format tag
    }

    const int num_patterns = std::min(128, max_pattern + 1);
    for (int p = 0; p < num_patterns; ++p) {
        engine::TrackerPattern pattern = engine::create_pattern(p, 64, channels);
        for (int r = 0; r < 64; ++r) {
            for (int ch = 0; ch < raw_channels; ++ch) {
                if (offset + 4 > size) {
                    results.warnings.emplace_back("pattern data truncated at pattern " +
                                                  std::to_string(p) + " row " + std::to_string(r) +
                                                  "; remaining cells empty");
                    offset = size;
                    continue;
                }
                const std::uint8_t b0 = data[offset];
                const std::uint8_t b1 = data[offset + 1];
                const std::uint8_t b2 = data[offset + 2];
                const std::uint8_t b3 = data[offset + 3];
                offset += 4;

                if (ch >= channels) {
                    continue; // clamped channel — cell dropped, count reported once
                }
                engine::TrackerCell& cell =
                    pattern.rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(ch)];
                const int period = ((b0 & 0x0F) << 8) | b1;
                cell.note = static_cast<std::uint8_t>(period_to_note(period));
                cell.instrument = static_cast<std::uint8_t>((b0 & 0xF0) | ((b2 & 0xF0) >> 4));
                cell.volume = 0xFF; // MOD has no volume column
                cell.effect = b2 & 0x0F;
                cell.effect_param = b3;
            }
        }
        project.patterns.push_back(std::move(pattern));
    }
    if (raw_channels > channels) {
        results.warnings.emplace_back("channels clamped from " + std::to_string(raw_channels) +
                                      " to " + std::to_string(channels));
    }

    // Initial BPM/speed from row-0 Fxx commands of the first played
    // pattern (common MOD convention).
    const int first_pattern = project.order_list.empty() ? 0 : project.order_list.front();
    if (first_pattern < static_cast<int>(project.patterns.size())) {
        for (const engine::TrackerCell& cell :
             project.patterns[static_cast<std::size_t>(first_pattern)].rows[0]) {
            if (cell.effect == 0xF && cell.effect_param > 0) {
                if (cell.effect_param >= 0x20) {
                    project.bpm = cell.effect_param;
                } else {
                    project.speed = cell.effect_param;
                }
            }
        }
    }

    // Sample PCM: 8-bit signed, upsampled; loop points scale with it.
    const double upsample_ratio = static_cast<double>(kModTargetRate) / kModSampleRate;
    for (int i = 0; i < num_sample_slots; ++i) {
        const RawSample& rs = raw_samples[static_cast<std::size_t>(i)];
        if (rs.length_bytes < 2) {
            continue;
        }
        if (offset + rs.length_bytes > size) {
            results.warnings.emplace_back("sample " + std::to_string(i + 1) + " (" + rs.name +
                                          ") truncated; remaining samples skipped");
            break;
        }
        const auto* pcm = reinterpret_cast<const std::int8_t*>(
            data + offset); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        ModWav wav = build_wav_mod(pcm, rs.length_bytes);
        offset += rs.length_bytes;

        const bool has_loop = rs.loop_length_bytes > 2;
        engine::TrackerSample sample;
        sample.id = i + 1;
        sample.name = upper(rs.name.empty() ? "SAMPLE " + std::to_string(i + 1) : rs.name);
        sample.name.resize(std::min<std::size_t>(sample.name.size(), 22));
        sample.file_name = sample.name + ".wav";
        sample.format = "wav";
        sample.original_data = std::move(wav.wav);
        sample.sample_rate = kModTargetRate;
        sample.num_channels = 1;
        sample.frames = wav.out_frames;
        sample.loop_start =
            has_loop ? static_cast<std::uint32_t>(std::lround(rs.loop_start_bytes * upsample_ratio))
                     : 0;
        sample.loop_length =
            has_loop
                ? static_cast<std::uint32_t>(std::lround(rs.loop_length_bytes * upsample_ratio))
                : 0;
        sample.base_note = kModBaseNote;
        sample.finetune = static_cast<int>(std::lround(rs.finetune * 12.5));
        sample.volume = rs.volume;
        sample.pan = 128;
        project.samples.push_back(std::move(sample));
    }

    return project;
}

} // namespace nt::io::import
