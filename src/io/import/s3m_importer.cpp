#include "io/import/s3m_importer.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace nt::io::import {

namespace {

std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// S3M letter effects (A=1…) → engine nibble commands. Returns false
// when the effect has no equivalent (counted by the caller).
bool map_effect(int cmd, int info, int& effect, int& param, int& dropped) {
    switch (cmd) {
    case 0:
        return false;
    case 1: // A set speed
        effect = 0xF;
        param = info & 0x1F;
        return true;
    case 2: // B jump to order
        effect = 0xB;
        param = info;
        return true;
    case 3: // C pattern break (decimal → BCD)
        effect = 0xD;
        param = ((info / 10) << 4) | (info % 10);
        return true;
    case 4: // D volume slide
        effect = 0xA;
        param = info;
        return true;
    case 5: // E porta down
        effect = 0x2;
        param = info;
        return true;
    case 6: // F porta up
        effect = 0x1;
        param = info;
        return true;
    case 7: // G tone porta
        effect = 0x3;
        param = info;
        return true;
    case 8: // H vibrato
        effect = 0x4;
        param = info;
        return true;
    case 9: // I tremor — no engine equivalent
        ++dropped;
        return false;
    case 10: // J arpeggio
        effect = 0x0;
        param = info;
        return true;
    case 11: // K vibrato + volume slide
        effect = 0x6;
        param = info;
        return true;
    case 12: // L porta + volume slide
        effect = 0x5;
        param = info;
        return true;
    case 15: // O sample offset
        effect = 0x9;
        param = info;
        return true;
    case 17: // Q retrigger → E9x
        effect = 0xE;
        param = 0x90 | (info & 0x0F);
        return true;
    case 18: // R tremolo
        effect = 0x7;
        param = info;
        return true;
    case 19: // S extended → Exy
        effect = 0xE;
        param = info;
        return true;
    case 20: // T set BPM
        effect = 0xF;
        param = info;
        return true;
    case 21: // U fine vibrato → vibrato
        effect = 0x4;
        param = info;
        return true;
    case 22: // V global volume → set volume
        effect = 0xC;
        param = std::min(0x40, info);
        return true;
    case 24: // X set panning
        effect = 0x8;
        param = info;
        return true;
    default:
        ++dropped;
        return false;
    }
}

} // namespace

std::optional<engine::TrackerProject> import_s3m(const std::uint8_t* data, std::size_t size,
                                                 ImportResults& results) {
    if (size < 96 || std::memcmp(data + 44, "SCRM", 4) != 0) {
        results.errors.emplace_back("not a valid S3M file");
        return std::nullopt;
    }

    engine::TrackerProject project;
    project.name = upper(read_string(data, 28));
    if (project.name.empty()) {
        project.name = "UNTITLED";
    }
    project.name.resize(std::min<std::size_t>(project.name.size(), 32));

    const int order_count = rd16(data + 32);
    const int inst_count = rd16(data + 34);
    const int pat_count = rd16(data + 36);
    project.speed = data[49] != 0 ? data[49] : 6;
    project.bpm = data[50] != 0 ? data[50] : 125;
    const bool samples_unsigned = rd16(data + 42) != 1;

    // Channel table: values < 16 are PCM channels.
    int channels = 0;
    for (int i = 0; i < 32; ++i) {
        if (data[64 + i] < 16) {
            channels = i + 1;
        }
    }
    channels = clamp_channels(std::max(1, channels));
    project.channels = channels;
    project.rows_per_pattern = 64;

    for (int i = 0; i < order_count; ++i) {
        const std::uint8_t o = data[96 + i];
        if (o != 0xFF && o != 0xFE) {
            project.order_list.push_back(o);
        }
    }
    if (project.order_list.empty()) {
        project.order_list.push_back(0);
        results.warnings.emplace_back("order list empty — defaulting to [0]");
    }

    const std::size_t para_base = 96 + static_cast<std::size_t>(order_count);
    std::vector<std::uint32_t> inst_paras;
    for (int i = 0; i < inst_count; ++i) {
        inst_paras.push_back(rd16(data + para_base + (static_cast<std::size_t>(i) * 2)));
    }
    std::vector<std::uint32_t> pat_paras;
    for (int i = 0; i < pat_count; ++i) {
        pat_paras.push_back(rd16(data + para_base + (static_cast<std::size_t>(inst_count) * 2) +
                                 (static_cast<std::size_t>(i) * 2)));
    }

    // Samples (instrument type 1 = PCM).
    int adpcm_skipped = 0;
    for (std::size_t i = 0; i < inst_paras.size() && project.samples.size() < 31; ++i) {
        const std::size_t base = static_cast<std::size_t>(inst_paras[i]) * 16;
        if (base == 0 || base + 80 > size) {
            continue;
        }
        if (data[base] != 1) {
            continue; // AdLib or empty slot
        }
        if (data[base + 30] != 0) {
            ++adpcm_skipped;
            continue;
        }
        const int flags = data[base + 31];
        const bool has_loop = (flags & 0x01) != 0;
        const bool is_stereo = (flags & 0x02) != 0;
        const bool is16 = (flags & 0x04) != 0;

        const std::size_t data_offset =
            (static_cast<std::size_t>(data[base + 13]) << 16U | rd16(data + base + 14)) * 16;
        const std::uint32_t length_bytes = rd32(data + base + 16);
        const std::uint32_t loop_start = rd32(data + base + 20);
        const std::uint32_t loop_end = rd32(data + base + 24);
        const int volume = std::min<int>(64, data[base + 28]);
        const std::uint32_t c5speed = std::max<std::uint32_t>(1, rd32(data + base + 32));
        const std::string sample_name = read_string(data + base + 48, 28);

        if (length_bytes == 0 || data_offset + length_bytes > size) {
            continue;
        }
        const int bytes_per = is16 ? 2 : 1;
        const int chans = is_stereo ? 2 : 1;
        const std::size_t frames = length_bytes / static_cast<std::size_t>(bytes_per * chans);
        if (frames == 0) {
            continue;
        }

        std::vector<float> float_data(frames);
        const std::uint8_t* pcm = data + data_offset;
        for (std::size_t f = 0; f < frames; ++f) {
            double acc = 0.0;
            for (int c = 0; c < chans; ++c) {
                const std::size_t at =
                    (f * static_cast<std::size_t>(chans)) + static_cast<std::size_t>(c);
                if (is16) {
                    const std::uint16_t raw = rd16(pcm + (at * 2));
                    const int val = samples_unsigned ? static_cast<int>(raw) - 32768
                                                     : static_cast<std::int16_t>(raw);
                    acc += static_cast<double>(val) / 32768.0;
                } else {
                    const int val = samples_unsigned ? static_cast<int>(pcm[at]) - 128
                                                     : static_cast<std::int8_t>(pcm[at]);
                    acc += static_cast<double>(val) / 128.0;
                }
            }
            float_data[f] = static_cast<float>(acc / chans);
        }

        engine::TrackerSample sample;
        sample.id = static_cast<int>(i) + 1;
        sample.name = upper(sample_name.empty() ? "SAMPLE " + std::to_string(i + 1) : sample_name);
        sample.name.resize(std::min<std::size_t>(sample.name.size(), 22));
        sample.file_name = sample.name + ".wav";
        sample.format = "wav";
        sample.original_data = build_wav_from_float(float_data, c5speed);
        sample.sample_rate = c5speed;
        sample.num_channels = 1;
        sample.frames = static_cast<std::uint32_t>(frames);
        sample.loop_start = has_loop ? loop_start : 0;
        sample.loop_length = (has_loop && loop_end > loop_start) ? loop_end - loop_start : 0;
        sample.base_note = 72; // MIDI C-5 at c5speed
        sample.volume = volume;
        sample.pan = 128;
        project.samples.push_back(std::move(sample));
    }
    if (adpcm_skipped > 0) {
        results.warnings.emplace_back(std::to_string(adpcm_skipped) +
                                      " ADPCM-packed samples skipped (not supported)");
    }

    // Patterns: run-length encoded rows.
    int dropped_effects = 0;
    for (int pi = 0; pi < pat_count; ++pi) {
        engine::TrackerPattern pattern = engine::create_pattern(pi, 64, channels);
        const std::size_t p_offset =
            static_cast<std::size_t>(pat_paras[static_cast<std::size_t>(pi)]) * 16;
        if (p_offset == 0 || p_offset + 2 > size) {
            project.patterns.push_back(std::move(pattern));
            continue;
        }
        std::size_t pos = p_offset + 2; // skip packed-length field
        for (int row = 0; row < 64; ++row) {
            while (pos < size) {
                const std::uint8_t flag = data[pos++];
                if (flag == 0) {
                    break; // end of row
                }
                const int ch = flag & 0x1F;
                int note = 0;
                int instrument = 0;
                int volume = 0xFF;
                int effect = 0;
                int param = 0;

                if ((flag & 0x20U) != 0 && pos + 2 <= size) {
                    const std::uint8_t note_byte = data[pos];
                    instrument = data[pos + 1];
                    pos += 2;
                    if (note_byte == 0xFE) {
                        note = engine::kNoteOff;
                    } else if (note_byte != 0xFF) {
                        note = (((note_byte >> 4) & 0x0F) * 12) + (note_byte & 0x0F) + 1;
                    }
                }
                if ((flag & 0x40U) != 0 && pos < size) {
                    const std::uint8_t vol = data[pos++];
                    volume = vol <= 64 ? vol : 0xFF;
                }
                if ((flag & 0x80U) != 0 && pos + 2 <= size) {
                    const int cmd = data[pos];
                    const int info = data[pos + 1];
                    pos += 2;
                    map_effect(cmd, info, effect, param, dropped_effects);
                }

                if (ch < channels && row < 64) {
                    engine::TrackerCell& cell =
                        pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(ch)];
                    cell.note = static_cast<std::uint8_t>(std::min(note, 97));
                    cell.instrument = static_cast<std::uint8_t>(instrument);
                    cell.volume = static_cast<std::uint8_t>(volume);
                    cell.effect = static_cast<std::uint8_t>(effect);
                    cell.effect_param = static_cast<std::uint8_t>(param);
                }
            }
        }
        project.patterns.push_back(std::move(pattern));
    }
    if (dropped_effects > 0) {
        results.warnings.emplace_back(std::to_string(dropped_effects) +
                                      " S3M effects had no equivalent (Tremor family) and "
                                      "were dropped");
    }

    // Row-0 tempo override scan.
    const int first_pattern = project.order_list.front();
    if (first_pattern < static_cast<int>(project.patterns.size())) {
        for (const auto& cell : project.patterns[static_cast<std::size_t>(first_pattern)].rows[0]) {
            if (cell.effect == 0xF && cell.effect_param > 0) {
                if (cell.effect_param >= 0x20) {
                    project.bpm = cell.effect_param;
                } else {
                    project.speed = cell.effect_param;
                }
            }
        }
    }

    return project;
}

} // namespace nt::io::import
