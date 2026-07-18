#include "io/import/it_importer.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
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

// IT letter effects → engine commands; approximations counted.
std::pair<int, int> map_effect(int cmd, int param, int& approximated) {
    switch (cmd) {
    case 1:
        return {0xF, param & 0x1F}; // A set speed
    case 2:
        return {0xB, param}; // B jump
    case 3:
        return {0xD, ((param / 10) << 4) | (param % 10)}; // C break (BCD)
    case 4:
        return {0xA, param}; // D vol slide
    case 5:
        return {0x2, param}; // E porta down
    case 6:
        return {0x1, param}; // F porta up
    case 7:
        return {0x3, param}; // G tone porta
    case 8:
        return {0x4, param}; // H vibrato
    case 9:
        ++approximated; // I tremor — dropped
        return {0, 0};
    case 10:
        return {0x0, param}; // J arpeggio
    case 11:
        return {0x6, param}; // K vib+vol
    case 12:
        return {0x5, param}; // L porta+vol
    case 13:
        ++approximated; // M channel volume → set volume
        return {0xC, std::min(0x40, param)};
    case 14:
        ++approximated; // N channel vol slide → vol slide
        return {0xA, param};
    case 15:
        return {0x9, param}; // O offset
    case 16:
        ++approximated; // P pan slide → set pan
        return {0x8, param};
    case 17:
        return {0xE, 0x90 | (param & 0x0F)}; // Q retrig
    case 18:
        return {0x7, param}; // R tremolo
    case 19:
        return {0xE, param}; // S extended
    case 20:
        return {0xF, std::max(0x20, param)}; // T set BPM
    case 21:
        ++approximated; // U fine vibrato → vibrato
        return {0x4, param};
    case 22:
        ++approximated; // V global volume → set volume
        return {0xC, std::min(0x40, param)};
    case 23:
        ++approximated; // W global vol slide → vol slide
        return {0xA, param};
    case 24:
        return {0x8, param}; // X set pan
    case 25:
        ++approximated; // Y panbrello → vibrato
        return {0x4, param};
    default:
        return {0, 0};
    }
}

// Variable-bit-width reader for IT compressed blocks.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint32_t read(int count) {
        std::uint32_t result = 0;
        int filled = 0;
        while (filled < count) {
            if (bits_left_ == 0) {
                if (pos_ >= size_) {
                    return result;
                }
                bits_ = data_[pos_++];
                bits_left_ = 8;
            }
            const int take = std::min(count - filled, bits_left_);
            const std::uint32_t mask = (1U << take) - 1;
            result |= (bits_ & mask) << filled;
            bits_ >>= static_cast<unsigned>(take);
            bits_left_ -= take;
            filled += take;
        }
        return result;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint32_t bits_ = 0;
    int bits_left_ = 0;
};

// IT 2.14/2.15 sample decompression (itsex.c parity). The accumulators
// wrap at their native widths; sub-width values sign-extend exactly as
// the reference's shift trick does.
std::vector<std::int8_t> it_decompress8(const std::uint8_t* src, std::size_t src_size,
                                        std::size_t num_samples, bool it215) {
    std::vector<std::int8_t> out(num_samples, 0);
    std::size_t out_pos = 0;
    std::size_t block_pos = 0;

    while (out_pos < num_samples) {
        if (block_pos + 2 > src_size) {
            break;
        }
        const std::size_t block_len = rd16(src + block_pos);
        block_pos += 2;
        if (block_len == 0 || block_pos + block_len > src_size) {
            break;
        }
        BitReader br(src + block_pos, block_len);
        block_pos += block_len;

        const std::size_t block_samples = std::min<std::size_t>(num_samples - out_pos, 0x8000);
        int left = 9;
        std::uint8_t temp = 0;
        std::uint8_t temp2 = 0;

        for (std::size_t pos = 0; pos < block_samples;) {
            const std::uint32_t bits = br.read(left);
            if (left < 7) {
                const std::uint32_t escape = 1U << (left - 1);
                if ((bits & 0xFFFFU) != escape) {
                    std::uint8_t val = 0;
                    if (left < 8) {
                        const int shift = 8 - left;
                        val = static_cast<std::uint8_t>(
                            static_cast<std::int8_t>(static_cast<std::uint8_t>(bits << shift)) >>
                            shift);
                    } else {
                        val = static_cast<std::uint8_t>(bits);
                    }
                    temp = static_cast<std::uint8_t>(temp + val);
                    temp2 = static_cast<std::uint8_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int8_t>(it215 ? temp2 : temp);
                    ++pos;
                } else {
                    const std::uint32_t new_left = (br.read(3) + 1) & 0xFF;
                    left = static_cast<int>(new_left < static_cast<std::uint32_t>(left)
                                                ? new_left
                                                : (new_left + 1) & 0xFF);
                    left = std::max(1, left); // corrupt escape guard
                }
            } else if (left < 9) {
                const std::uint32_t upper_bound = ((0xFFU >> (9 - left)) + 4) & 0xFFFF;
                const std::uint32_t lower_bound = (upper_bound - 8) & 0xFFFF;
                if (bits <= lower_bound || bits > upper_bound) {
                    std::uint8_t val = 0;
                    if (left < 8) {
                        const int shift = 8 - left;
                        val = static_cast<std::uint8_t>(
                            static_cast<std::int8_t>(static_cast<std::uint8_t>(bits << shift)) >>
                            shift);
                    } else {
                        val = static_cast<std::uint8_t>(bits);
                    }
                    temp = static_cast<std::uint8_t>(temp + val);
                    temp2 = static_cast<std::uint8_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int8_t>(it215 ? temp2 : temp);
                    ++pos;
                } else {
                    const std::uint32_t new_left = (bits - lower_bound) & 0xFF;
                    left = static_cast<int>(new_left < static_cast<std::uint32_t>(left)
                                                ? new_left
                                                : (new_left + 1) & 0xFF);
                    left = std::max(1, left); // corrupt escape guard
                }
            } else {
                if (left >= 10) {
                    ++pos; // invalid width — skip position
                } else if (bits >= 256) {
                    left = std::max(1, static_cast<int>((bits + 1) & 0xFF));
                } else {
                    temp = static_cast<std::uint8_t>(temp + bits);
                    temp2 = static_cast<std::uint8_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int8_t>(it215 ? temp2 : temp);
                    ++pos;
                }
            }
            if (out_pos >= num_samples) {
                break;
            }
        }
    }
    return out;
}

std::vector<std::int16_t> it_decompress16(const std::uint8_t* src, std::size_t src_size,
                                          std::size_t num_samples, bool it215) {
    std::vector<std::int16_t> out(num_samples, 0);
    std::size_t out_pos = 0;
    std::size_t block_pos = 0;

    while (out_pos < num_samples) {
        if (block_pos + 2 > src_size) {
            break;
        }
        const std::size_t block_len = rd16(src + block_pos);
        block_pos += 2;
        if (block_len == 0 || block_pos + block_len > src_size) {
            break;
        }
        BitReader br(src + block_pos, block_len);
        block_pos += block_len;

        const std::size_t block_samples = std::min<std::size_t>(num_samples - out_pos, 0x4000);
        int left = 17;
        std::uint16_t temp = 0;
        std::uint16_t temp2 = 0;

        for (std::size_t pos = 0; pos < block_samples;) {
            const std::uint32_t bits = br.read(left);
            if (left < 7) {
                const std::uint32_t escape = 1U << (left - 1);
                if (bits != escape) {
                    std::uint16_t val = 0;
                    if (left < 16) {
                        const int shift = 16 - left;
                        val = static_cast<std::uint16_t>(
                            static_cast<std::int16_t>(static_cast<std::uint16_t>(bits << shift)) >>
                            shift);
                    } else {
                        val = static_cast<std::uint16_t>(bits);
                    }
                    temp = static_cast<std::uint16_t>(temp + val);
                    temp2 = static_cast<std::uint16_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int16_t>(it215 ? temp2 : temp);
                    ++pos;
                } else {
                    const std::uint32_t new_left = (br.read(4) + 1) & 0xFF;
                    left = static_cast<int>(new_left < static_cast<std::uint32_t>(left)
                                                ? new_left
                                                : (new_left + 1) & 0xFF);
                    left = std::max(1, left); // corrupt escape guard
                }
            } else if (left < 17) {
                const std::uint32_t upper_bound = ((0xFFFFU >> (17 - left)) + 8) & 0xFFFF;
                const std::uint32_t lower_bound = (upper_bound - 16) & 0xFFFF;
                if (bits <= lower_bound || bits > upper_bound) {
                    std::uint16_t val = 0;
                    if (left < 16) {
                        const int shift = 16 - left;
                        val = static_cast<std::uint16_t>(
                            static_cast<std::int16_t>(static_cast<std::uint16_t>(bits << shift)) >>
                            shift);
                    } else {
                        val = static_cast<std::uint16_t>(bits);
                    }
                    temp = static_cast<std::uint16_t>(temp + val);
                    temp2 = static_cast<std::uint16_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int16_t>(it215 ? temp2 : temp);
                    ++pos;
                } else {
                    const std::uint32_t new_left = (bits - lower_bound) & 0xFF;
                    left = static_cast<int>(new_left < static_cast<std::uint32_t>(left)
                                                ? new_left
                                                : (new_left + 1) & 0xFF);
                    left = std::max(1, left); // corrupt escape guard
                }
            } else {
                if (left >= 18) {
                    ++pos;
                } else if (bits >= 0x10000) {
                    left = std::max(1, static_cast<int>((bits + 1) & 0xFF));
                } else {
                    temp = static_cast<std::uint16_t>(temp + bits);
                    temp2 = static_cast<std::uint16_t>(temp2 + temp);
                    out[out_pos++] = static_cast<std::int16_t>(it215 ? temp2 : temp);
                    ++pos;
                }
            }
            if (out_pos >= num_samples) {
                break;
            }
        }
    }
    return out;
}

} // namespace

std::optional<engine::TrackerProject> import_it(const std::uint8_t* data, std::size_t size,
                                                ImportResults& results) {
    if (size < 192 || std::memcmp(data, "IMPM", 4) != 0) {
        results.errors.emplace_back("not a valid IT file");
        return std::nullopt;
    }

    engine::TrackerProject project;
    project.name = upper(read_string(data + 4, 26));
    if (project.name.empty()) {
        project.name = "UNTITLED";
    }
    project.name.resize(std::min<std::size_t>(project.name.size(), 32));

    const int order_count = rd16(data + 32);
    const int instr_count = rd16(data + 34);
    const int sample_count = rd16(data + 36);
    const int pattern_count = rd16(data + 38);
    const int cwtv = rd16(data + 40);
    const int cmwt = rd16(data + 42);
    const int flags = rd16(data + 44);
    const bool use_instruments = (flags & 0x04) != 0;
    project.speed = data[50] != 0 ? data[50] : 6;
    project.bpm = data[51] != 0 ? data[51] : 125;
    const bool it215 = cmwt >= 0x0215;
    const bool cwtv_default_signed = cwtv >= 0x0202;

    int channels = 0;
    for (int i = 0; i < 64; ++i) {
        if (data[64 + i] != 0xFF) {
            channels = i + 1;
        }
    }
    channels = clamp_channels(std::max(1, channels));
    project.channels = channels;
    project.rows_per_pattern = 64;

    for (int i = 0; i < order_count; ++i) {
        const std::uint8_t o = data[192 + i];
        if (o != 0xFF && o != 0xFE) {
            project.order_list.push_back(o);
        }
    }
    if (project.order_list.empty()) {
        project.order_list.push_back(0);
    }

    const std::size_t instr_start = 192 + static_cast<std::size_t>(order_count);
    std::vector<std::uint32_t> instr_offsets;
    instr_offsets.reserve(static_cast<std::size_t>(instr_count));
    for (int i = 0; i < instr_count; ++i) {
        instr_offsets.push_back(rd32(data + instr_start + (static_cast<std::size_t>(i) * 4)));
    }
    const std::size_t sample_start = instr_start + (static_cast<std::size_t>(instr_count) * 4);
    std::vector<std::uint32_t> sample_offsets;
    sample_offsets.reserve(static_cast<std::size_t>(sample_count));
    for (int i = 0; i < sample_count; ++i) {
        sample_offsets.push_back(rd32(data + sample_start + (static_cast<std::size_t>(i) * 4)));
    }
    const std::size_t pat_start = sample_start + (static_cast<std::size_t>(sample_count) * 4);
    std::vector<std::uint32_t> pat_offsets;
    pat_offsets.reserve(static_cast<std::size_t>(pattern_count));
    for (int i = 0; i < pattern_count; ++i) {
        pat_offsets.push_back(rd32(data + pat_start + (static_cast<std::size_t>(i) * 4)));
    }

    // Samples.
    int decompression_failures = 0;
    for (std::size_t i = 0; i < sample_offsets.size() && project.samples.size() < 31; ++i) {
        const std::size_t base = sample_offsets[i];
        if (base == 0 || base + 80 > size || std::memcmp(data + base, "IMPS", 4) != 0) {
            continue;
        }
        const int global_vol = std::min<int>(64, data[base + 17]);
        const int sflags = data[base + 18];
        if ((sflags & 0x01) == 0) {
            continue; // no sample data
        }
        const bool is16 = (sflags & 0x02) != 0;
        const bool is_stereo = (sflags & 0x04) != 0;
        const bool compressed = (sflags & 0x08) != 0;
        const bool has_loop = (sflags & 0x10) != 0;
        const int default_vol = std::min<int>(64, data[base + 19]);
        const std::string sample_name = read_string(data + base + 20, 26);
        const int convert = data[base + 46];
        const bool is_signed = (convert & 0x01) != 0 || (convert == 0 && cwtv_default_signed);
        const int pan_byte = data[base + 47];
        const int pan = (pan_byte & 0x80) != 0
                            ? static_cast<int>(std::lround((pan_byte & 0x7F) * 255.0 / 64.0))
                            : 128;
        const std::uint32_t frames = rd32(data + base + 48);
        const std::uint32_t loop_begin = rd32(data + base + 52);
        const std::uint32_t loop_end = rd32(data + base + 56);
        const std::uint32_t c5speed = std::max<std::uint32_t>(1, rd32(data + base + 60));
        const std::size_t data_offset = rd32(data + base + 72);

        if (frames == 0 || data_offset == 0 || data_offset >= size) {
            continue;
        }
        const int chans = is_stereo ? 2 : 1;
        const std::size_t total_values =
            static_cast<std::size_t>(frames) * static_cast<std::size_t>(chans);

        std::vector<float> float_data(frames, 0.0F);
        if (compressed) {
            const std::size_t avail = size - data_offset;
            if (is16) {
                const auto pcm = it_decompress16(data + data_offset, avail, total_values, it215);
                for (std::uint32_t f = 0; f < frames; ++f) {
                    if (is_stereo) {
                        const std::size_t at = static_cast<std::size_t>(f) * 2;
                        float_data[f] =
                            (static_cast<float>(pcm[at]) + static_cast<float>(pcm[at + 1])) / 2.0F /
                            32768.0F;
                    } else {
                        float_data[f] = static_cast<float>(pcm[f]) / 32768.0F;
                    }
                }
            } else {
                const auto pcm = it_decompress8(data + data_offset, avail, total_values, it215);
                for (std::uint32_t f = 0; f < frames; ++f) {
                    if (is_stereo) {
                        const std::size_t at = static_cast<std::size_t>(f) * 2;
                        float_data[f] =
                            (static_cast<float>(pcm[at]) + static_cast<float>(pcm[at + 1])) / 2.0F /
                            128.0F;
                    } else {
                        float_data[f] = static_cast<float>(pcm[f]) / 128.0F;
                    }
                }
            }
            // A short decompress leaves trailing silence; the tally
            // surfaces it (per-block recovery mirroring the web path).
            if (total_values > 0 && avail < total_values / 8) {
                ++decompression_failures;
            }
        } else {
            const std::size_t bytes_per =
                static_cast<std::size_t>(is16 ? 2 : 1) * static_cast<std::size_t>(chans);
            if (data_offset + (static_cast<std::size_t>(frames) * bytes_per) > size) {
                continue;
            }
            const std::uint8_t* pcm = data + data_offset;
            for (std::uint32_t f = 0; f < frames; ++f) {
                double acc = 0.0;
                for (int c = 0; c < chans; ++c) {
                    const std::size_t at =
                        (static_cast<std::size_t>(f) * static_cast<std::size_t>(chans)) +
                        static_cast<std::size_t>(c);
                    if (is16) {
                        const std::uint16_t raw = rd16(pcm + (at * 2));
                        const int val = is_signed ? static_cast<std::int16_t>(raw)
                                                  : static_cast<int>(raw) - 32768;
                        acc += static_cast<double>(val) / 32768.0;
                    } else {
                        const int val = is_signed ? static_cast<std::int8_t>(pcm[at])
                                                  : static_cast<int>(pcm[at]) - 128;
                        acc += static_cast<double>(val) / 128.0;
                    }
                }
                float_data[f] = static_cast<float>(acc / chans);
            }
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
        sample.frames = frames;
        sample.loop_start = has_loop ? loop_begin : 0;
        sample.loop_length = (has_loop && loop_end > loop_begin) ? loop_end - loop_begin : 0;
        sample.base_note = 72; // MIDI C-5 at c5speed
        sample.volume = default_vol > 0 ? default_vol : global_vol;
        sample.pan = pan;
        project.samples.push_back(std::move(sample));
    }
    if (decompression_failures > 0) {
        results.warnings.emplace_back(std::to_string(decompression_failures) +
                                      " compressed samples appear truncated (trailing "
                                      "silence substituted)");
    }

    // Instrument keyboard tables → sample slots (instrument mode).
    std::map<int, int> instr_to_sample;
    if (use_instruments) {
        for (std::size_t i = 0; i < instr_offsets.size(); ++i) {
            const std::size_t base = instr_offsets[i];
            if (base == 0 || base + 184 > size || std::memcmp(data + base, "IMPI", 4) != 0) {
                continue;
            }
            const std::size_t table = base + 64;
            int sample_id = data[table + 121]; // C-5 keymap entry (60*2 + 1)
            if (sample_id == 0 || sample_id > sample_count) {
                for (int n = 0; n < 120; ++n) {
                    const int s = data[table + (static_cast<std::size_t>(n) * 2) + 1];
                    if (s > 0 && s <= sample_count) {
                        sample_id = s;
                        break;
                    }
                }
            }
            instr_to_sample[static_cast<int>(i) + 1] = sample_id;
        }
    }

    // Patterns: channel-masked RLE with per-channel memory.
    int approximated = 0;
    for (int pi = 0; pi < pattern_count; ++pi) {
        engine::TrackerPattern pattern = engine::create_pattern(pi, 64, channels);
        const std::size_t pat_offset = pat_offsets[static_cast<std::size_t>(pi)];
        if (pat_offset != 0 && pat_offset + 8 <= size) {
            const int data_len = rd16(data + pat_offset);
            const int row_count = rd16(data + pat_offset + 2);
            const std::size_t data_start = pat_offset + 8;
            if (data_start + static_cast<std::size_t>(data_len) <= size) {
                pattern = engine::create_pattern(pi, row_count, channels);
                std::size_t pos = data_start;
                const std::size_t data_end = data_start + static_cast<std::size_t>(data_len);
                std::array<std::uint8_t, 64> last_mask{};
                std::array<std::uint8_t, 64> last_note{};
                std::array<std::uint8_t, 64> last_instr{};
                std::array<std::uint8_t, 64> last_vol{};
                std::array<std::uint8_t, 64> last_cmd{};
                std::array<std::uint8_t, 64> last_val{};

                for (int row = 0; row < row_count; ++row) {
                    while (pos < data_end) {
                        const std::uint8_t chan_var = data[pos++];
                        if (chan_var == 0) {
                            break;
                        }
                        const int ch = (chan_var - 1) & 0x3F;
                        std::uint8_t mask = 0;
                        if ((chan_var & 0x80U) != 0 && pos < data_end) {
                            mask = data[pos++];
                            last_mask[static_cast<std::size_t>(ch)] = mask;
                        } else {
                            mask = last_mask[static_cast<std::size_t>(ch)];
                        }

                        int note = 0;
                        int instrument = 0;
                        int volume = 0xFF;
                        int effect = 0;
                        int param = 0;
                        if ((mask & 0x01U) != 0 && pos < data_end) {
                            note = data[pos++];
                            last_note[static_cast<std::size_t>(ch)] =
                                static_cast<std::uint8_t>(note);
                        }
                        if ((mask & 0x02U) != 0 && pos < data_end) {
                            instrument = data[pos++];
                            last_instr[static_cast<std::size_t>(ch)] =
                                static_cast<std::uint8_t>(instrument);
                        }
                        if ((mask & 0x04U) != 0 && pos < data_end) {
                            const std::uint8_t v = data[pos++];
                            last_vol[static_cast<std::size_t>(ch)] = v;
                            volume = v <= 64 ? v : 0xFF;
                        }
                        if ((mask & 0x08U) != 0 && pos + 2 <= data_end) {
                            const std::uint8_t cmd = data[pos];
                            const std::uint8_t val = data[pos + 1];
                            pos += 2;
                            last_cmd[static_cast<std::size_t>(ch)] = cmd;
                            last_val[static_cast<std::size_t>(ch)] = val;
                            const auto [e, p] = map_effect(cmd, val, approximated);
                            effect = e;
                            param = p;
                        }
                        if ((mask & 0x10U) != 0) {
                            note = last_note[static_cast<std::size_t>(ch)];
                        }
                        if ((mask & 0x20U) != 0) {
                            instrument = last_instr[static_cast<std::size_t>(ch)];
                        }
                        if ((mask & 0x40U) != 0) {
                            const std::uint8_t v = last_vol[static_cast<std::size_t>(ch)];
                            volume = v <= 64 ? v : 0xFF;
                        }
                        if ((mask & 0x80U) != 0) {
                            const auto [e, p] =
                                map_effect(last_cmd[static_cast<std::size_t>(ch)],
                                           last_val[static_cast<std::size_t>(ch)], approximated);
                            effect = e;
                            param = p;
                        }

                        if (ch < channels && row < row_count) {
                            engine::TrackerCell& cell = pattern.rows[static_cast<std::size_t>(row)]
                                                                    [static_cast<std::size_t>(ch)];
                            if (note == 255 || note == 254) {
                                cell.note = engine::kNoteOff;
                            } else if (note >= 1 && note <= 119) {
                                cell.note =
                                    static_cast<std::uint8_t>(std::min(engine::kMaxNote, note));
                            }
                            cell.instrument = static_cast<std::uint8_t>(instrument);
                            cell.volume = static_cast<std::uint8_t>(volume);
                            cell.effect = static_cast<std::uint8_t>(effect);
                            cell.effect_param = static_cast<std::uint8_t>(param);
                        }
                    }
                }
            }
        }
        project.patterns.push_back(std::move(pattern));
    }
    if (project.patterns.empty()) {
        project.patterns.push_back(engine::create_pattern(0, 64, channels));
    }
    if (approximated > 0) {
        results.warnings.emplace_back(
            std::to_string(approximated) +
            " IT effects approximated or dropped (Tremor/Panbrello/global-volume family)");
    }

    // Instrument-mode remap to flat sample slots.
    if (use_instruments && !instr_to_sample.empty()) {
        for (auto& pattern : project.patterns) {
            for (auto& row : pattern.rows) {
                for (auto& cell : row) {
                    if (cell.instrument > 0) {
                        const auto it = instr_to_sample.find(cell.instrument);
                        cell.instrument =
                            static_cast<std::uint8_t>(it != instr_to_sample.end() ? it->second : 0);
                    }
                }
            }
        }
    }

    // Row-0 tempo scan.
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
