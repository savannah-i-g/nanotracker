#include "io/import/xm_importer.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nt::io::import {

namespace {

constexpr int kMidiC5 = 72; // XM reference pitch (8363 Hz at C-5)
constexpr int kXmSampleRate = 8363;

std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

// Effect remap tallies so the report can say exactly what happened.
struct EffectStats {
    int approximated = 0;
    int dropped = 0;
};

// XM effects 0x00-0x0F are the engine's own set; extended effects
// remap with explicit approximation accounting.
std::pair<int, int> map_effect(int fx_type, int fx_param, EffectStats& stats) {
    if (fx_type <= 0x0F) {
        return {fx_type, fx_param};
    }
    switch (fx_type) {
    case 0x10: // Gxx set global volume → set volume (approx)
        ++stats.approximated;
        return {0xC, std::min(0x40, fx_param)};
    case 0x11: // Hxy global volume slide → volume slide (approx)
        ++stats.approximated;
        return {0xA, fx_param};
    case 0x14: // Kxx key off → volume 0 (approx)
        ++stats.approximated;
        return {0xC, 0x00};
    case 0x19: // Pxy panning slide → set pan (approx)
        ++stats.approximated;
        return {0x8, fx_param};
    case 0x1B: // Rxy retrigger+volume → E9x retrig
        ++stats.approximated;
        return {0xE, 0x90 | (fx_param & 0x0F)};
    case 0x1D: // Txy tremor → volume slide (approx; engine has no tremor)
        ++stats.approximated;
        return {0xA, fx_param};
    case 0x21: { // Xxy extra-fine portamento → fine portamento (approx)
        const int sub = (fx_param >> 4) & 0x0F;
        const int val = fx_param & 0x0F;
        if (sub == 1) {
            ++stats.approximated;
            return {0xE, 0x10 | val};
        }
        if (sub == 2) {
            ++stats.approximated;
            return {0xE, 0x20 | val};
        }
        ++stats.dropped;
        return {0, 0};
    }
    default:
        ++stats.dropped;
        return {0, 0};
    }
}

struct VolumeColumn {
    int volume = 0xFF;
    int effect = 0;
    int param = 0;
};

VolumeColumn map_volume(int vol_byte) {
    if (vol_byte < 0x10) {
        return {};
    }
    if (vol_byte <= 0x50) {
        return {.volume = vol_byte - 0x10, .effect = 0, .param = 0};
    }
    switch (vol_byte >> 4) {
    case 0x6:
        return {.volume = 0xFF, .effect = 0xA, .param = vol_byte & 0x0F};
    case 0x7:
        return {.volume = 0xFF, .effect = 0xA, .param = (vol_byte & 0x0F) << 4};
    case 0xC:
        return {.volume = 0xFF, .effect = 0x8, .param = (vol_byte & 0x0F) * 17};
    case 0xF:
        return {.volume = 0xFF, .effect = 0x3, .param = vol_byte & 0x0F};
    default:
        return {};
    }
}

std::vector<std::int8_t> delta_decode8(const std::uint8_t* src, std::size_t n) {
    std::vector<std::int8_t> out(n);
    int acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc = (acc + src[i]) & 0xFF;
        out[i] = static_cast<std::int8_t>(acc < 128 ? acc : acc - 256);
    }
    return out;
}

std::vector<std::int16_t> delta_decode16(const std::uint8_t* src, std::size_t frames) {
    std::vector<std::int16_t> out(frames);
    int acc = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto delta = static_cast<std::int16_t>(rd16(src + (i * 2)));
        acc = (acc + delta) & 0xFFFF;
        out[i] = static_cast<std::int16_t>(acc < 32768 ? acc : acc - 65536);
    }
    return out;
}

} // namespace

std::optional<engine::TrackerProject> import_xm(const std::uint8_t* data, std::size_t size,
                                                ImportResults& results) {
    if (size < 80 || std::memcmp(data, "Extended Module: ", 17) != 0) {
        results.errors.emplace_back("not a valid XM file");
        return std::nullopt;
    }

    engine::TrackerProject project;
    project.name = read_string(data + 17, 20);
    for (char& c : project.name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (project.name.empty()) {
        project.name = "UNTITLED";
    }

    // Version check (fix #1): the format standard is 0x0104; other
    // versions load best-effort with a loud warning.
    const int version = rd16(data + 58);
    if (version != 0x0104) {
        results.warnings.emplace_back("XM version " + std::to_string(version) +
                                      " is not the standard 0x0104; loading best-effort");
    }

    const std::uint32_t header_size = rd32(data + 60);
    const int song_length = rd16(data + 64);
    const int file_channels = rd16(data + 68);
    const int channels = clamp_channels(file_channels);
    const int num_patterns = rd16(data + 70);
    const int num_instruments = rd16(data + 72);
    const int flags = rd16(data + 74);
    project.speed = rd16(data + 76) != 0 ? rd16(data + 76) : 6;
    project.bpm = rd16(data + 78) != 0 ? rd16(data + 78) : 125;
    project.channels = channels;
    if ((flags & 1) != 0) {
        project.freq_table = engine::FreqTable::kLinear;
    }

    for (int i = 0; i < std::min(song_length, 256); ++i) {
        project.order_list.push_back(data[80 + i]);
    }
    if (project.order_list.empty()) {
        project.order_list.push_back(0);
    }

    EffectStats stats;
    int clamped_notes = 0;

    // Patterns.
    std::size_t pos = 60 + header_size;
    for (int pi = 0; pi < num_patterns; ++pi) {
        if (pos + 9 > size) {
            results.warnings.emplace_back("pattern data truncated at pattern " +
                                          std::to_string(pi));
            break;
        }
        const std::uint32_t pat_header = rd32(data + pos);
        const int row_count = rd16(data + pos + 5) != 0 ? rd16(data + pos + 5) : 64;
        const int data_size = rd16(data + pos + 7);
        std::size_t p = pos + pat_header;
        const std::size_t data_end = std::min(size, p + static_cast<std::size_t>(data_size));

        engine::TrackerPattern pattern = engine::create_pattern(pi, row_count, channels);
        for (int row = 0; row < row_count; ++row) {
            for (int ch = 0; ch < file_channels; ++ch) {
                int note = 0;
                int instrument = 0;
                int vol_byte = 0;
                int fx_type = 0;
                int fx_param = 0;
                if (p < data_end) {
                    const std::uint8_t first = data[p];
                    if ((first & 0x80U) != 0) {
                        ++p;
                        if ((first & 0x01U) != 0 && p < data_end) {
                            note = data[p++];
                        }
                        if ((first & 0x02U) != 0 && p < data_end) {
                            instrument = data[p++];
                        }
                        if ((first & 0x04U) != 0 && p < data_end) {
                            vol_byte = data[p++];
                        }
                        if ((first & 0x08U) != 0 && p < data_end) {
                            fx_type = data[p++];
                        }
                        if ((first & 0x10U) != 0 && p < data_end) {
                            fx_param = data[p++];
                        }
                    } else if (p + 5 <= data_end) {
                        note = data[p];
                        instrument = data[p + 1];
                        vol_byte = data[p + 2];
                        fx_type = data[p + 3];
                        fx_param = data[p + 4];
                        p += 5;
                    } else {
                        p = data_end;
                    }
                }

                if (ch >= channels) {
                    continue;
                }
                engine::TrackerCell& cell =
                    pattern.rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(ch)];
                if (note == engine::kNoteOff) {
                    cell.note = engine::kNoteOff;
                } else if (note >= 1 && note <= engine::kMaxNote) {
                    cell.note = static_cast<std::uint8_t>(note);
                } else if (note != 0) {
                    ++clamped_notes;
                }
                cell.instrument = static_cast<std::uint8_t>(instrument);

                const VolumeColumn vc = map_volume(vol_byte);
                cell.volume = static_cast<std::uint8_t>(vc.volume);
                if (fx_type != 0 || fx_param != 0) {
                    const auto [effect, param] = map_effect(fx_type, fx_param, stats);
                    cell.effect = static_cast<std::uint8_t>(effect);
                    cell.effect_param = static_cast<std::uint8_t>(param);
                } else if (vc.effect != 0) {
                    cell.effect = static_cast<std::uint8_t>(vc.effect);
                    cell.effect_param = static_cast<std::uint8_t>(vc.param);
                }
            }
        }
        project.patterns.push_back(std::move(pattern));
        pos += pat_header + static_cast<std::size_t>(data_size);
    }
    project.rows_per_pattern = 64;
    for (const auto& pattern : project.patterns) {
        project.rows_per_pattern =
            std::max(project.rows_per_pattern, static_cast<int>(pattern.rows.size()));
    }

    // Instruments → flat sample slots + per-note keymaps.
    std::map<int, std::vector<int>> instr_to_slots;
    std::map<int, std::array<std::uint8_t, 96>> instr_keymap;
    int slot_id = 1;
    for (int instr = 1; instr <= num_instruments && slot_id <= engine::kMaxSamples; ++instr) {
        if (pos + 4 > size) {
            break;
        }
        const std::uint32_t instr_header = rd32(data + pos);
        const std::size_t instr_end = pos + instr_header;
        const int num_sub = pos + 29 <= size ? rd16(data + pos + 27) : 0;
        if (num_sub == 0) {
            instr_to_slots[instr] = {};
            instr_keymap[instr] = {};
            pos = instr_end;
            continue;
        }

        std::array<std::uint8_t, 96> keymap{};
        if (instr_header >= 129 && pos + 33 + 96 <= size) {
            std::memcpy(keymap.data(), data + pos + 33, 96);
        }
        instr_keymap[instr] = keymap;

        // FT2 ignores the stored sample-header-size field; the
        // standard 40 bytes is the interoperable read (OpenMPT wiki).
        constexpr std::size_t kSampleHeader = 40;
        const std::size_t headers_start = pos + instr_header;

        struct SubSample {
            std::uint32_t length = 0;
            std::uint32_t loop_start = 0;
            std::uint32_t loop_length = 0;
            int volume = 64;
            int finetune = 0;
            int flags = 0;
            int panning = 128;
            int rel_note = 0;
            std::string name;
            std::size_t data_offset = 0;
            bool is16 = false;
        };

        std::vector<SubSample> subs;
        std::size_t sh = headers_start;
        for (int si = 0; si < num_sub; ++si) {
            if (sh + kSampleHeader > size) {
                break;
            }
            SubSample ss;
            ss.length = rd32(data + sh);
            ss.loop_start = rd32(data + sh + 4);
            ss.loop_length = rd32(data + sh + 8);
            ss.volume = std::min<int>(64, data[sh + 12]);
            ss.finetune = static_cast<std::int8_t>(data[sh + 13]);
            ss.flags = data[sh + 14];
            ss.panning = data[sh + 15];
            ss.rel_note = static_cast<std::int8_t>(data[sh + 16]);
            ss.name = read_string(data + sh + 18, 22);
            ss.is16 = (ss.flags & 0x10) != 0;
            subs.push_back(std::move(ss));
            sh += kSampleHeader;
        }

        std::size_t data_pos = headers_start + (static_cast<std::size_t>(num_sub) * kSampleHeader);
        std::vector<int> slot_ids;
        for (SubSample& ss : subs) {
            if (slot_id > engine::kMaxSamples) {
                results.warnings.emplace_back(
                    "sample slots exhausted (31); remaining sub-samples dropped");
                break;
            }
            ss.data_offset = data_pos;
            data_pos += ss.length;
            if (ss.length == 0 || ss.data_offset + ss.length > size) {
                slot_ids.push_back(0);
                continue;
            }
            const std::size_t frames = ss.is16 ? ss.length / 2 : ss.length;

            std::vector<float> float_data(frames);
            if (ss.is16) {
                const std::vector<std::int16_t> pcm = delta_decode16(data + ss.data_offset, frames);
                for (std::size_t i = 0; i < frames; ++i) {
                    float_data[i] = static_cast<float>(pcm[i]) / 32768.0F;
                }
            } else {
                const std::vector<std::int8_t> pcm = delta_decode8(data + ss.data_offset, frames);
                for (std::size_t i = 0; i < frames; ++i) {
                    float_data[i] = static_cast<float>(pcm[i]) / 128.0F;
                }
            }

            engine::TrackerSample sample;
            sample.id = slot_id;
            sample.name = ss.name.empty() ? "SAMPLE " + std::to_string(slot_id) : ss.name;
            for (char& c : sample.name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            sample.name.resize(std::min<std::size_t>(sample.name.size(), 22));
            sample.file_name = sample.name + ".wav";
            sample.format = "wav";
            sample.original_data = build_wav_from_float(float_data, kXmSampleRate);
            sample.sample_rate = kXmSampleRate;
            sample.num_channels = 1;
            sample.frames = static_cast<std::uint32_t>(frames);

            const int frame_div = ss.is16 ? 2 : 1;
            const bool has_loop = (ss.flags & 0x03) != 0 && ss.loop_length > 0;
            const auto raw_ls = ss.loop_start / static_cast<std::uint32_t>(frame_div);
            const auto raw_ll = ss.loop_length / static_cast<std::uint32_t>(frame_div);
            sample.loop_start =
                has_loop ? std::min<std::uint32_t>(raw_ls, static_cast<std::uint32_t>(frames)) : 0;
            sample.loop_length =
                has_loop ? std::min<std::uint32_t>(raw_ll, static_cast<std::uint32_t>(frames) -
                                                               sample.loop_start)
                         : 0;

            // relNote folds into base_note: positive relNote tunes the
            // sample UP, so the engine reference note goes DOWN
            // (base_note = C5 - relNote). Sign pinned by regression
            // test — the web app once shipped the inverse.
            sample.base_note = kMidiC5 - ss.rel_note;
            // 1/128-semitone units → cents.
            sample.finetune =
                std::clamp(static_cast<int>(std::lround(ss.finetune * 100.0 / 128.0)), -128, 127);
            sample.volume = ss.volume;
            sample.pan = ss.panning;
            project.samples.push_back(std::move(sample));
            slot_ids.push_back(slot_id++);
        }
        instr_to_slots[instr] = std::move(slot_ids);
        pos = data_pos;
    }

    // Remap (instrument, note) pairs to flat sample slots via keymaps.
    for (auto& pattern : project.patterns) {
        for (auto& row : pattern.rows) {
            for (auto& cell : row) {
                if (cell.instrument == 0) {
                    continue;
                }
                const auto slots_it = instr_to_slots.find(cell.instrument);
                if (slots_it == instr_to_slots.end() || slots_it->second.empty()) {
                    cell.instrument = 0;
                    continue;
                }
                const auto& slots = slots_it->second;
                int resolved = slots[0];
                if (cell.note >= 1 && cell.note <= engine::kMaxNote) {
                    const auto& keymap = instr_keymap[cell.instrument];
                    const std::uint8_t sub = keymap[static_cast<std::size_t>(cell.note - 1)];
                    resolved = sub < slots.size() ? slots[sub] : slots[0];
                }
                cell.instrument = static_cast<std::uint8_t>(std::max(0, resolved));
            }
        }
    }

    // Row-0 Fxx tempo scan (same convention as the MOD importer).
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

    if (stats.approximated > 0) {
        results.warnings.emplace_back(std::to_string(stats.approximated) +
                                      " XM effects approximated (global volume/key-off/"
                                      "tremor/pan-slide family)");
    }
    if (stats.dropped > 0) {
        results.warnings.emplace_back(std::to_string(stats.dropped) +
                                      " XM effects had no equivalent and were dropped");
    }
    if (clamped_notes > 0) {
        results.warnings.emplace_back(std::to_string(clamped_notes) +
                                      " out-of-range notes cleared");
    }

    return project;
}

} // namespace nt::io::import
