#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace nt::engine {

namespace {

// Classic MOD periods for octave 4, notes C..B.
constexpr std::array<int, 12> kPeriodTableBase = {1712, 1616, 1524, 1440, 1356, 1280,
                                                  1208, 1140, 1076, 1016, 960,  906};

// 64-step sine for vibrato/tremolo, amplitude ±255 (matches the web
// engine's rounded table exactly — the golden traces depend on it).
std::array<int, 64> make_sine_table() {
    std::array<int, 64> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        constexpr double kTau = 6.283185307179586;
        table[i] =
            static_cast<int>(std::round(std::sin((static_cast<double>(i) / 64.0) * kTau) * 255.0));
    }
    return table;
}

const std::array<int, 64> kSineTable = make_sine_table();

const TrackerCell* cell_at(const TrackerPattern& pattern, int row, int channel) {
    if (row < 0 || row >= static_cast<int>(pattern.rows.size())) {
        return nullptr;
    }
    const auto& cells = pattern.rows[static_cast<std::size_t>(row)];
    if (channel < 0 || channel >= static_cast<int>(cells.size())) {
        return nullptr;
    }
    return &cells[static_cast<std::size_t>(channel)];
}

const TrackerPattern* pattern_by_id(const TrackerProject& project, int id) {
    // Pattern ids are array indices in practice (createProject/createPattern
    // number them sequentially), but lookup stays id-based for safety.
    if (id >= 0 && id < static_cast<int>(project.patterns.size()) &&
        project.patterns[static_cast<std::size_t>(id)].id == id) {
        return &project.patterns[static_cast<std::size_t>(id)];
    }
    for (const TrackerPattern& p : project.patterns) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

int pattern_rows_for_order(const TrackerProject& project, int order_pos, int fallback) {
    if (order_pos < 0 || order_pos >= static_cast<int>(project.order_list.size())) {
        return fallback;
    }
    const TrackerPattern* p =
        pattern_by_id(project, project.order_list[static_cast<std::size_t>(order_pos)]);
    return p != nullptr ? static_cast<int>(p->rows.size()) : fallback;
}

void push_seq_trigger(TrackerPlayState& state, const SequenceTrigger& trigger) {
    if (state.seq_trigger_count >= kMaxSeqTriggersPerTick) {
        ++state.seq_triggers_dropped;
        return;
    }
    state.seq_triggers[static_cast<std::size_t>(state.seq_trigger_count++)] = trigger;
}

// Binary search + scan mirroring the web seq engine's
// findNotesStartingAtTick / findNotesEndingAtTick.
void scan_sequence_layers(TrackerPlayState& state, const TrackerProject& project, int pattern_index,
                          int absolute_tick) {
    const auto& seq_patterns = project.sequence_mixer.seq_patterns;
    if (pattern_index < 0 || pattern_index >= static_cast<int>(seq_patterns.size())) {
        return;
    }
    const SequencePattern& seq = seq_patterns[static_cast<std::size_t>(pattern_index)];
    for (int ch = 0; ch < static_cast<int>(seq.layers.size()); ++ch) {
        const auto& layers = seq.layers[static_cast<std::size_t>(ch)];
        for (int li = 0; li < static_cast<int>(layers.size()); ++li) {
            const SequenceLayer& layer = layers[static_cast<std::size_t>(li)];
            if (!layer.enabled) {
                continue;
            }
            const auto& notes = layer.notes;

            // Notes starting at this tick (lower_bound on start_tick).
            std::size_t lo = 0;
            std::size_t hi = notes.size();
            while (lo < hi) {
                const std::size_t mid = (lo + hi) / 2;
                if (notes[mid].start_tick < absolute_tick) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            for (std::size_t i = lo; i < notes.size() && notes[i].start_tick == absolute_tick;
                 ++i) {
                push_seq_trigger(state, {.channel_index = ch,
                                         .layer_index = li,
                                         .pitch = notes[i].pitch,
                                         .velocity = notes[i].velocity,
                                         .is_note_on = true,
                                         .instrument = layer.instrument});
            }

            // Notes whose natural end lands on this tick.
            for (const SequenceNote& n : notes) {
                if (n.start_tick >= absolute_tick) {
                    break;
                }
                if (n.start_tick + n.duration_ticks == absolute_tick) {
                    push_seq_trigger(state, {.channel_index = ch,
                                             .layer_index = li,
                                             .pitch = n.pitch,
                                             .velocity = 0,
                                             .is_note_on = false,
                                             .instrument = layer.instrument});
                }
            }
        }
    }
}

} // namespace

int note_to_period(int tracker_note) {
    if (tracker_note <= 0 || tracker_note > kMaxNote) {
        return 0;
    }
    const int n = tracker_note - 1;
    const int octave = n / 12;
    const int semitone = n % 12;
    const double base = kPeriodTableBase[static_cast<std::size_t>(semitone)];
    return static_cast<int>(std::round(base * std::pow(2.0, 4 - octave)));
}

double period_to_playback_rate(int period, int sample_base_note) {
    if (period == 0) {
        return 1.0;
    }
    // Tracker note 1 = C-0 = MIDI 12.
    const int tracker_base = std::clamp(sample_base_note - 11, 1, kMaxNote);
    const int ref_period = note_to_period(tracker_base);
    if (ref_period == 0) {
        return 1.0;
    }
    return static_cast<double>(ref_period) / period;
}

int track_bound_instruments(const TrackerProject& project, int channel_index, int* out,
                            int capacity) {
    int count = 0;
    for (int i = 0; i < static_cast<int>(project.instrument_table.size()); ++i) {
        const auto& bt = project.instrument_table[static_cast<std::size_t>(i)].bound_tracks;
        if (std::find(bt.begin(), bt.end(), channel_index) != bt.end()) {
            if (count < capacity) {
                out[count] = i + 1;
            }
            ++count;
        }
    }
    return std::min(count, capacity);
}

int resolve_bound_instrument(const TrackerProject& project, int channel_index, int bound_index) {
    std::array<int, kMaxSamples> bound{};
    const int count = track_bound_instruments(project, channel_index, bound.data(), kMaxSamples);
    if (count == 0) {
        return 0;
    }
    const int idx = std::max(0, bound_index);
    return idx < count ? bound[static_cast<std::size_t>(idx)] : 0;
}

TrackerPattern create_pattern(int id, int row_count, int channel_count) {
    TrackerPattern pattern;
    pattern.id = id;
    std::array<char, 8> name{};
    std::snprintf(name.data(), name.size(), "PAT%02d", id);
    pattern.name = name.data();
    pattern.rows.assign(static_cast<std::size_t>(row_count),
                        std::vector<TrackerCell>(static_cast<std::size_t>(channel_count)));
    return pattern;
}

TrackerProject create_project(int channels) {
    TrackerProject project;
    project.channels = channels;
    project.patterns.push_back(create_pattern(0, project.rows_per_pattern, channels));
    project.order_list = {0};
    return project;
}

TrackerPlayState create_play_state(const TrackerProject& project) {
    TrackerPlayState state;
    state.bpm = project.bpm;
    state.speed = project.speed;
    state.pattern_index = project.order_list.empty() ? 0 : project.order_list.front();
    return state;
}

void advance_tick(TrackerPlayState& state, const TrackerProject& project) {
    if (!state.is_playing || state.is_paused) {
        return;
    }

    // Per-tick output flags reset (the web engine builds fresh channel
    // copies each tick; the persistent effect memory carries over).
    const int channel_count = std::min(project.channels, kMaxChannels);
    for (int ch = 0; ch < channel_count; ++ch) {
        ChannelPlayState& c = state.channels[static_cast<std::size_t>(ch)];
        c.trigger_note = false;
        c.release_note = false;
        c.period_override = 0;
        c.volume_override = -1;
    }
    state.pattern_ended = false;
    state.song_ended = false;
    state.fx_automation_cell = nullptr;
    state.seq_trigger_count = 0;
    bool has_loop_jump = false; // produced and consumed within this call
    state.pattern_loop_flush = false;
    state.row_fired = false;

    const TrackerPattern* pattern = pattern_by_id(project, state.pattern_index);
    if (pattern == nullptr) {
        return;
    }

    int& row = state.row;
    int& tick = state.tick;

    if (tick == 0 && state.pattern_delay == 0) {
        // Row trigger: notes + row-trigger effects.
        state.row_fired = true;
        for (int ch = 0; ch < channel_count; ++ch) {
            const TrackerCell* cell = cell_at(*pattern, row, ch);
            if (cell == nullptr) {
                continue;
            }
            ChannelPlayState& c = state.channels[static_cast<std::size_t>(ch)];

            const bool has_note = cell->note > 0 && cell->note <= kMaxNote;
            const bool is_note_off = cell->note == kNoteOff;
            int effective_instrument = cell->instrument;
            if (effective_instrument == 0 && has_note) {
                effective_instrument = resolve_bound_instrument(project, ch, cell->bound_index);
            }
            const bool has_instrument = effective_instrument > 0;
            const int effect = cell->effect;
            const int param = cell->effect_param;

            c.row_effect = effect;
            c.row_effect_param = param;

            // Fxx first so this row already runs at the new speed/BPM.
            if (effect == 0xF) {
                if (param < 0x20) {
                    state.speed = std::clamp(param == 0 ? 1 : param, 1, 31);
                } else {
                    state.bpm = std::clamp(param, 20, 255);
                }
            }

            if (is_note_off) {
                c.release_note = true;
                c.note = 0;
            }

            const InstrumentTableEntry* ins_entry =
                (has_instrument && !project.instrument_table.empty() &&
                 effective_instrument <= static_cast<int>(project.instrument_table.size()))
                    ? &project.instrument_table[static_cast<std::size_t>(effective_instrument - 1)]
                    : nullptr;
            const InstrumentSourceType ins_type =
                ins_entry != nullptr ? ins_entry->type : InstrumentSourceType::kSample;
            const bool is_sample_ins = ins_type == InstrumentSourceType::kSample;
            const int resolved_sample_id =
                (ins_entry != nullptr && ins_entry->type == InstrumentSourceType::kSample)
                    ? ins_entry->sample_id
                    : effective_instrument;
            const TrackerSample* sample = nullptr;
            if (is_sample_ins) {
                for (const TrackerSample& s : project.samples) {
                    if (s.id == resolved_sample_id) {
                        sample = &s;
                        break;
                    }
                }
            }

            if (has_note && effect != 0x3 && effect != 0x5) {
                if (has_instrument) {
                    c.instrument = effective_instrument;
                    c.instrument_type = ins_type;
                    if (sample != nullptr) {
                        c.volume = cell->volume != 0xFF ? cell->volume : sample->volume;
                        c.pan = sample->pan;
                    } else if (!is_sample_ins) {
                        c.volume = cell->volume != 0xFF ? cell->volume : 64;
                    }
                } else if (cell->volume != 0xFF) {
                    c.volume = cell->volume;
                }
                c.note = cell->note;
                c.period = note_to_period(cell->note);
                c.vibrato_phase = 0;
                c.tremolo_phase = 0;
                c.sample_offset = 0;
                c.trigger_note = true;
            } else if (has_note) {
                // Tone portamento target (3xx / 5xx): glide, no retrigger.
                c.porta_target = note_to_period(cell->note);
                if (has_instrument) {
                    c.instrument = effective_instrument;
                    c.instrument_type = ins_type;
                }
                if (param != 0) {
                    c.porta_speed = param;
                }
            }

            if (!has_note && has_instrument && effect != 0x3 && effect != 0x5) {
                c.instrument = effective_instrument;
                c.instrument_type = ins_type;
                if (sample != nullptr) {
                    c.volume = sample->volume;
                    c.pan = sample->pan;
                }
            }

            switch (effect) {
            case 0x0: // Arpeggio — param 0 resets (000 = off)
                c.arpeggio0 = 0;
                c.arpeggio1 = (param >> 4) & 0xF;
                c.arpeggio2 = param & 0xF;
                break;
            case 0x4: // Vibrato
                if (((param >> 4) & 0xF) != 0) {
                    c.vibrato_speed = (param >> 4) & 0xF;
                }
                if ((param & 0xF) != 0) {
                    c.vibrato_depth = param & 0xF;
                }
                break;
            case 0x7: // Tremolo
                if (((param >> 4) & 0xF) != 0) {
                    c.tremolo_speed = (param >> 4) & 0xF;
                }
                if ((param & 0xF) != 0) {
                    c.tremolo_depth = param & 0xF;
                }
                break;
            case 0x8: // Set panning
                c.pan = param;
                break;
            case 0x9: // Sample offset
                if (param != 0) {
                    c.sample_offset = param * 256;
                }
                break;
            case 0xA: // Volume slide — sign-encoded delta for later ticks
                c.vol_slide = (param >> 4) != 0 ? (param >> 4) : -(param & 0xF);
                break;
            case 0xB: // Position jump
                state.has_jump = true;
                state.next_order_pos =
                    std::min(param, static_cast<int>(project.order_list.size()) - 1);
                state.next_row = 0;
                break;
            case 0xC: // Set volume
                c.volume = std::min(64, param);
                break;
            case 0xD: // Pattern break (Bxx wins when both present)
                if (!state.has_jump) {
                    state.has_jump = true;
                    state.next_order_pos = state.order_pos + 1;
                    state.next_row = ((param >> 4) * 10) + (param & 0xF);
                }
                break;
            case 0xE: {
                const int sub = (param >> 4) & 0xF;
                const int val = param & 0xF;
                switch (sub) {
                case 0x1:
                    c.porta_speed = val;
                    break;
                case 0x2:
                    c.porta_speed = -val;
                    break;
                case 0x5:
                    c.finetune = val;
                    break;
                case 0x6: // Pattern loop
                    if (val == 0) {
                        state.loop_row = row;
                    } else if (state.loop_count == 0) {
                        state.loop_count = val;
                        state.loop_active = true;
                        has_loop_jump = true;
                    } else {
                        --state.loop_count;
                        if (state.loop_count == 0) {
                            state.loop_active = false;
                        } else {
                            has_loop_jump = true;
                        }
                    }
                    break;
                case 0xE: // Pattern delay
                    if (val > 0) {
                        state.pattern_delay = val;
                    }
                    break;
                case 0x9: // Retrig
                    c.retrig_param = val;
                    c.retrig_count = 0;
                    break;
                case 0xA:
                    c.volume = std::min(64, c.volume + val);
                    break;
                case 0xB:
                    c.volume = std::max(0, c.volume - val);
                    break;
                case 0xC: // Note cut — fires on the matching later tick
                    c.retrig_param = 0xC0 | val;
                    break;
                case 0xD: // Note delay
                    if (has_note && val != 0) {
                        c.trigger_note = false;
                        c.retrig_param = 0xD0 | val;
                    }
                    break;
                default:
                    break;
                }
                break;
            }
            default:
                break;
            }
        }

        // FX automation cell on this row.
        const auto& fx_patterns = project.fx_mixer.fx_patterns;
        if (state.pattern_index >= 0 &&
            state.pattern_index < static_cast<int>(fx_patterns.size())) {
            const FxPattern& fx = fx_patterns[static_cast<std::size_t>(state.pattern_index)];
            if (row < static_cast<int>(fx.present.size()) &&
                fx.present[static_cast<std::size_t>(row)]) {
                state.fx_automation_cell = &fx.cells[static_cast<std::size_t>(row)];
            }
        }
    } else if (tick > 0 || state.pattern_delay > 0) {
        // Mid-row (or pattern-delay extra) ticks: continuous effects.
        for (int ch = 0; ch < channel_count; ++ch) {
            const TrackerCell* cell = cell_at(*pattern, row, ch);
            if (cell == nullptr) {
                continue;
            }
            ChannelPlayState& c = state.channels[static_cast<std::size_t>(ch)];
            const int effect = cell->effect;
            const int param = cell->effect_param;

            switch (effect) {
            case 0x0: { // Arpeggio
                int semi = c.arpeggio2;
                if (tick % 3 == 0) {
                    semi = c.arpeggio0;
                } else if (tick % 3 == 1) {
                    semi = c.arpeggio1;
                }
                c.period_override = semi != 0 ? note_to_period(c.note + semi) : 0;
                break;
            }
            case 0x1: // Porta up
                c.period = std::max(113, c.period - param);
                break;
            case 0x2: // Porta down
                c.period = std::min(856, c.period + param);
                break;
            case 0x3:
            case 0x5: { // Tone porta (+ volume slide for 5xx)
                const int spd = c.porta_speed;
                if (c.period < c.porta_target) {
                    c.period = std::min(c.porta_target, c.period + spd);
                } else if (c.period > c.porta_target) {
                    c.period = std::max(c.porta_target, c.period - spd);
                }
                if (effect == 0x5) {
                    c.volume = std::clamp(c.volume + c.vol_slide, 0, 64);
                }
                break;
            }
            case 0x4:
            case 0x6: { // Vibrato (+ volume slide for 6xx)
                c.vibrato_phase = (c.vibrato_phase + c.vibrato_speed) & 0x3F;
                const int vib = static_cast<int>(std::round(
                    static_cast<double>(kSineTable[static_cast<std::size_t>(c.vibrato_phase)] *
                                        c.vibrato_depth) /
                    128.0));
                c.period_override = std::max(1, c.period + vib);
                if (effect == 0x6) {
                    c.volume = std::clamp(c.volume + c.vol_slide, 0, 64);
                }
                break;
            }
            case 0x7: { // Tremolo
                c.tremolo_phase = (c.tremolo_phase + c.tremolo_speed) & 0x3F;
                const int trem = static_cast<int>(std::round(
                    static_cast<double>(kSineTable[static_cast<std::size_t>(c.tremolo_phase)] *
                                        c.tremolo_depth) /
                    128.0));
                c.volume_override = std::clamp(c.volume + trem, 0, 64);
                break;
            }
            case 0xA: // Volume slide
                c.volume = std::clamp(c.volume + c.vol_slide, 0, 64);
                break;
            case 0xE: {
                const int sub = (param >> 4) & 0xF;
                const int val = param & 0xF;
                if (sub == 0xC && tick == val) { // Note cut
                    c.volume = 0;
                    c.release_note = true;
                }
                if (sub == 0xD && tick == val) { // Note delay
                    c.trigger_note = true;
                }
                if (sub == 0x9 && c.retrig_param > 0) { // Retrig
                    ++c.retrig_count;
                    if (c.retrig_count >= c.retrig_param) {
                        c.retrig_count = 0;
                        c.trigger_note = true;
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

    // Sequence layers scan before the tick advances (tick-0 notes fire).
    scan_sequence_layers(state, project, state.pattern_index, row * state.speed + tick);

    // Advance tick / row / order.
    ++tick;
    if (tick >= state.speed) {
        tick = 0;

        const auto advance_row = [&]() {
            ++row;
            const int row_count = static_cast<int>(pattern->rows.size());
            if (row >= row_count ||
                (state.has_jump && row >= state.next_row && !state.loop_active)) {
                if (state.has_jump) {
                    state.order_pos = static_cast<int>(
                        static_cast<std::size_t>(state.next_order_pos) % project.order_list.size());
                    row = std::min(state.next_row,
                                   pattern_rows_for_order(project, state.order_pos, 64) - 1);
                    state.pattern_index =
                        project.order_list[static_cast<std::size_t>(state.order_pos)];
                    state.has_jump = false;
                    state.next_order_pos = 0;
                    state.next_row = 0;
                    state.loop_row = 0;
                    state.loop_count = 0;
                    state.loop_active = false;
                } else {
                    state.pattern_ended = true;
                    ++state.order_pos;
                    if (state.order_pos >= static_cast<int>(project.order_list.size())) {
                        state.order_pos = 0;
                        state.song_ended = true;
                    }
                    state.pattern_index =
                        project.order_list[static_cast<std::size_t>(state.order_pos)];
                    row = 0;
                    state.loop_row = 0;
                    state.loop_count = 0;
                    state.loop_active = false;
                }
            }
        };

        if (state.pattern_delay > 0) {
            --state.pattern_delay;
            if (state.pattern_delay == 0) {
                if (has_loop_jump) {
                    row = state.loop_row;
                    state.pattern_loop_flush = true;
                } else {
                    advance_row();
                }
            }
        } else if (has_loop_jump) {
            row = state.loop_row;
            state.pattern_loop_flush = true;
        } else {
            advance_row();
        }
    }

    state.has_loop_jump = has_loop_jump;
}

const char* note_name(int note, char* out) {
    static constexpr std::array<const char*, 12> kNames = {"C-", "C#", "D-", "D#", "E-", "F-",
                                                           "F#", "G-", "G#", "A-", "A#", "B-"};
    if (note == 0) {
        std::memcpy(out, "---", 4);
        return out;
    }
    if (note == kNoteOff) {
        std::memcpy(out, "===", 4);
        return out;
    }
    const int n = note - 1;
    const int octave = n / 12;
    const int semitone = n % 12;
    out[0] = kNames[static_cast<std::size_t>(semitone)][0];
    out[1] = kNames[static_cast<std::size_t>(semitone)][1];
    out[2] = static_cast<char>('0' + octave);
    out[3] = '\0';
    return out;
}

int name_to_note(const char* name) {
    static constexpr std::array<const char*, 12> kNames = {"C-", "C#", "D-", "D#", "E-", "F-",
                                                           "F#", "G-", "G#", "A-", "A#", "B-"};
    if (name == nullptr || name[0] == '\0' || std::strncmp(name, "---", 3) == 0) {
        return 0;
    }
    if (std::strncmp(name, "===", 3) == 0) {
        return kNoteOff;
    }
    for (int i = 0; i < 12; ++i) {
        if (name[0] == kNames[static_cast<std::size_t>(i)][0] &&
            name[1] == kNames[static_cast<std::size_t>(i)][1]) {
            const char oct = name[2];
            if (oct < '0' || oct > '7') {
                return 0;
            }
            return i + ((oct - '0') * 12) + 1;
        }
    }
    return 0;
}

} // namespace nt::engine
