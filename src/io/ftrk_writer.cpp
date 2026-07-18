#include "io/ftrk_writer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace nt::io {

namespace {

constexpr int kWriteVersion = 15;

// Little-endian append helpers mirroring the reader's Cursor.
struct Out {
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t v) { bytes.push_back(v); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }

    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v & 0xFFFF));
        u16(static_cast<std::uint16_t>((v >> 16) & 0xFFFF));
    }

    void f32(float v) {
        std::uint32_t raw = 0;
        std::memcpy(&raw, &v, sizeof(raw));
        u32(raw);
    }

    void f64(double v) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, &v, sizeof(raw));
        u32(static_cast<std::uint32_t>(raw & 0xFFFFFFFF));
        u32(static_cast<std::uint32_t>(raw >> 32));
    }

    // Fixed-width field: truncated/zero-padded (reader trims at NUL).
    void fixed_string(const std::string& s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            u8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
        }
    }

    void magic(const char* four) { bytes.insert(bytes.end(), four, four + 4); }

    void blob(const std::vector<std::uint8_t>& data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    void str8(const std::string& s) {
        u8(static_cast<std::uint8_t>(std::min<std::size_t>(s.size(), 255)));
        for (std::size_t i = 0; i < std::min<std::size_t>(s.size(), 255); ++i) {
            u8(static_cast<std::uint8_t>(s[i]));
        }
    }

    void str16(const std::string& s) {
        const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFF));
        u16(n);
        for (std::size_t i = 0; i < n; ++i) {
            u8(static_cast<std::uint8_t>(s[i]));
        }
    }

    void str32(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }

    void patch_u32(std::size_t at, std::uint32_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        bytes[at + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        bytes[at + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    }
};

std::uint8_t format_code(const std::string& format) {
    if (format == "ogg") {
        return 1;
    }
    if (format == "mp3") {
        return 2;
    }
    return 0; // wav
}

} // namespace

std::vector<std::uint8_t> write_ftrk(const engine::TrackerProject& project,
                                     const FtrkWriteExtras& extras) {
    Out out;
    out.magic("FTRK");
    out.u16(kWriteVersion);
    out.fixed_string(project.name, 32);
    out.f32(static_cast<float>(project.bpm));
    out.u8(static_cast<std::uint8_t>(project.speed));
    out.u8(static_cast<std::uint8_t>(project.rows_per_pattern));
    out.u8(static_cast<std::uint8_t>(project.channels));
    out.u8(static_cast<std::uint8_t>(project.patterns.size()));
    out.u16(static_cast<std::uint16_t>(project.order_list.size()));
    out.u8(static_cast<std::uint8_t>(project.samples.size()));

    // Reserved region (v15: 39 bytes — nine u32 block offsets + 3
    // spare). Offsets patch in as blocks land.
    const std::size_t reserved_pos = out.bytes.size();
    for (int i = 0; i < 39; ++i) {
        out.u8(0);
    }

    for (const int order : project.order_list) {
        out.u8(static_cast<std::uint8_t>(order));
    }

    for (const engine::TrackerPattern& pattern : project.patterns) {
        out.fixed_string(pattern.name, 16);
        out.u16(static_cast<std::uint16_t>(pattern.rows.size()));
        out.u8(static_cast<std::uint8_t>(pattern.highlight_major));
        out.u8(static_cast<std::uint8_t>(pattern.highlight_minor));
        for (const auto& row : pattern.rows) {
            for (int ch = 0; ch < project.channels; ++ch) {
                const engine::TrackerCell cell = ch < static_cast<int>(row.size())
                                                     ? row[static_cast<std::size_t>(ch)]
                                                     : engine::TrackerCell{};
                out.u8(cell.note);
                out.u8(static_cast<std::uint8_t>((cell.instrument & 0x1F) |
                                                 ((cell.bound_index & 0x07) << 5)));
                out.u8(cell.volume);
                out.u8(cell.effect);
                out.u8(cell.effect_param);
            }
        }
    }

    for (const engine::TrackerSample& sample : project.samples) {
        out.u8(static_cast<std::uint8_t>(sample.id));
        out.u8(format_code(sample.format));
        out.u8(static_cast<std::uint8_t>(sample.base_note));
        out.u8(static_cast<std::uint8_t>(
            static_cast<std::int8_t>(std::clamp(sample.finetune, -128, 127))));
        out.u8(static_cast<std::uint8_t>(sample.volume));
        out.u8(static_cast<std::uint8_t>(sample.pan));
        out.u32(sample.loop_start);
        out.u32(sample.loop_length);
        out.u32(sample.sample_rate);
        out.u8(static_cast<std::uint8_t>(sample.num_channels));
        out.u32(sample.frames);
        out.u32(static_cast<std::uint32_t>(sample.original_data.size()));
        out.fixed_string(sample.name, 22);
        out.fixed_string(sample.file_name, 32);
        out.u16(
            static_cast<std::uint16_t>(std::clamp(sample.stretch_ratio * 10000.0, 0.0, 65535.0)));
        out.u8(static_cast<std::uint8_t>(sample.category));
    }
    for (const engine::TrackerSample& sample : project.samples) {
        out.blob(sample.original_data);
    }

    // ── FXMX ─────────────────────────────────────────────────────────
    out.patch_u32(reserved_pos, static_cast<std::uint32_t>(out.bytes.size()));
    out.magic("FXMX");
    out.u8(static_cast<std::uint8_t>(project.fx_mixer.channels.size()));
    for (const engine::FxChannelStrip& strip : project.fx_mixer.channels) {
        out.fixed_string(strip.name, 16);
        out.u8(static_cast<std::uint8_t>(strip.volume));
        out.u8(static_cast<std::uint8_t>(static_cast<std::int8_t>(strip.pan)));
        out.u8(strip.enabled ? 1 : 0);
        // Colour: packed RGB back to "#rrggbb" (empty when unset).
        std::string colour;
        if (strip.color != 0) {
            std::array<char, 8> hex{};
            std::snprintf(hex.data(), hex.size(), "#%06x",
                          static_cast<unsigned>(strip.color & 0xFFFFFFU));
            colour = hex.data();
        }
        out.str8(colour);
        out.u8(static_cast<std::uint8_t>(strip.tracker_sends.size()));
        for (const float send : strip.tracker_sends) {
            out.f32(send);
        }
        out.u8(static_cast<std::uint8_t>(strip.modules.size()));
        for (const engine::FxModuleInstance& module : strip.modules) {
            out.fixed_string(module.module_id, 16);
            out.u8(module.enabled ? 1 : 0);
            out.u8(static_cast<std::uint8_t>(module.params.size()));
            for (const auto& [key, value] : module.params) {
                out.str8(key);
                out.f32(value);
            }
        }
    }
    out.u8(static_cast<std::uint8_t>(project.fx_mixer.fx_patterns.size()));
    for (const engine::FxPattern& fx : project.fx_mixer.fx_patterns) {
        out.u16(static_cast<std::uint16_t>(fx.present.size()));
        for (std::size_t r = 0; r < fx.present.size(); ++r) {
            if (!fx.present[r]) {
                out.u8(0);
                continue;
            }
            const engine::FxCell& cell = fx.cells[r];
            out.u8(1);
            out.u8(static_cast<std::uint8_t>(cell.channel_index));
            out.u8(static_cast<std::uint8_t>(cell.module_index));
            out.str8(cell.param_key);
            out.f32(static_cast<float>(cell.value));
        }
    }

    // ── INTB (+BNDT) ─────────────────────────────────────────────────
    if (!project.instrument_table.empty()) {
        out.patch_u32(reserved_pos + 4, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("INTB");
        out.u8(static_cast<std::uint8_t>(project.instrument_table.size()));
        for (const engine::InstrumentTableEntry& entry : project.instrument_table) {
            switch (entry.type) {
            case engine::InstrumentSourceType::kSample:
                out.u8(0);
                out.u8(static_cast<std::uint8_t>(entry.sample_id));
                break;
            case engine::InstrumentSourceType::kPlugin:
                out.u8(1);
                out.str8(entry.plugin_id);
                out.u8(static_cast<std::uint8_t>(entry.plugin_preset_params.size()));
                for (const auto& [key, value] : entry.plugin_preset_params) {
                    out.str8(key);
                    out.f32(static_cast<float>(value));
                }
                break;
            case engine::InstrumentSourceType::kWorkspace:
                out.u8(2);
                out.str16(entry.workspace_id);
                break;
            }
        }
        out.magic("BNDT");
        for (const engine::InstrumentTableEntry& entry : project.instrument_table) {
            out.u8(static_cast<std::uint8_t>(entry.bound_tracks.size()));
            for (const int track : entry.bound_tracks) {
                out.u8(static_cast<std::uint8_t>(track));
            }
        }
    }

    // ── WPBR ─────────────────────────────────────────────────────────
    if (!extras.workspace_json.empty()) {
        out.patch_u32(reserved_pos + 8, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("WPBR");
        out.str32(extras.workspace_json);
    }

    // ── PLGB ─────────────────────────────────────────────────────────
    if (!extras.plugins.empty()) {
        out.patch_u32(reserved_pos + 12, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("PLGB");
        out.u16(static_cast<std::uint16_t>(extras.plugins.size()));
        for (const FtrkBundledPlugin& plugin : extras.plugins) {
            out.str16(plugin.plugin_id);
            out.u32(static_cast<std::uint32_t>(plugin.archive.size()));
            out.blob(plugin.archive);
        }
    }

    // ── SEQB (+ sparse channel colors) ───────────────────────────────
    const auto& seq_patterns = project.sequence_mixer.seq_patterns;
    out.patch_u32(reserved_pos + 16, static_cast<std::uint32_t>(out.bytes.size()));
    out.magic("SEQB");
    out.u16(static_cast<std::uint16_t>(seq_patterns.size()));
    out.u8(static_cast<std::uint8_t>(project.channels));
    for (const engine::SequencePattern& sp : seq_patterns) {
        for (int ch = 0; ch < project.channels; ++ch) {
            const auto* layers = ch < static_cast<int>(sp.layers.size())
                                     ? &sp.layers[static_cast<std::size_t>(ch)]
                                     : nullptr;
            out.u8(static_cast<std::uint8_t>(layers != nullptr ? layers->size() : 0));
            if (layers == nullptr) {
                continue;
            }
            for (const engine::SequenceLayer& layer : *layers) {
                out.u8(static_cast<std::uint8_t>(layer.instrument));
                out.u8(layer.enabled ? 1 : 0);
                out.u16(static_cast<std::uint16_t>(layer.notes.size()));
                for (const engine::SequenceNote& note : layer.notes) {
                    out.u8(static_cast<std::uint8_t>(note.pitch));
                    out.u16(static_cast<std::uint16_t>(note.start_tick));
                    out.u16(static_cast<std::uint16_t>(note.duration_ticks));
                    out.u8(static_cast<std::uint8_t>(note.velocity));
                }
            }
        }
    }
    int colour_count = 0;
    for (int ch = 0; ch < project.channels; ++ch) {
        if (project.channel_colors[static_cast<std::size_t>(ch)] != 0) {
            ++colour_count;
        }
    }
    out.u8(static_cast<std::uint8_t>(colour_count));
    for (int ch = 0; ch < project.channels; ++ch) {
        const std::uint32_t colour = project.channel_colors[static_cast<std::size_t>(ch)];
        if (colour != 0) {
            out.u8(static_cast<std::uint8_t>(ch));
            out.u8(static_cast<std::uint8_t>((colour >> 16) & 0xFF));
            out.u8(static_cast<std::uint8_t>((colour >> 8) & 0xFF));
            out.u8(static_cast<std::uint8_t>(colour & 0xFF));
        }
    }

    // ── POVR (plugin sample overrides, block v1 — web shape) ─────────
    if (!extras.povr.empty()) {
        out.patch_u32(reserved_pos + 20, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("POVR");
        out.u8(1); // block version
        // Group by instance id, first-appearance order (the web
        // serialiser's Map-insertion grouping).
        std::vector<std::string> instance_ids;
        for (const FtrkPovrOverride& entry : extras.povr) {
            if (std::find(instance_ids.begin(), instance_ids.end(), entry.instance_id) ==
                instance_ids.end()) {
                instance_ids.push_back(entry.instance_id);
            }
        }
        out.u16(static_cast<std::uint16_t>(instance_ids.size()));
        // Dedup: each unique hash's bytes appear once; later
        // references carry byteLen 0 and the reader re-expands.
        std::vector<std::string> seen_hashes;
        for (const std::string& instance_id : instance_ids) {
            out.str16(instance_id);
            std::uint16_t slot_count = 0;
            for (const FtrkPovrOverride& entry : extras.povr) {
                if (entry.instance_id == instance_id) {
                    ++slot_count;
                }
            }
            out.u16(slot_count);
            for (const FtrkPovrOverride& entry : extras.povr) {
                if (entry.instance_id != instance_id) {
                    continue;
                }
                out.str16(entry.slot_id);
                out.str16(entry.hash);
                out.str16(entry.name);
                out.u32(entry.sample_rate);
                out.u8(entry.channels);
                out.f32(entry.duration);
                const bool first = std::find(seen_hashes.begin(), seen_hashes.end(), entry.hash) ==
                                   seen_hashes.end();
                if (first) {
                    seen_hashes.push_back(entry.hash);
                    out.u32(static_cast<std::uint32_t>(entry.bytes.size()));
                    out.blob(entry.bytes);
                } else {
                    out.u32(0);
                }
            }
        }
    } else if (!extras.povr_raw.empty()) {
        // A block the reader could not interpret round-trips verbatim.
        out.patch_u32(reserved_pos + 20, static_cast<std::uint32_t>(out.bytes.size()));
        out.blob(extras.povr_raw);
    }

    // ── PPRS ─────────────────────────────────────────────────────────
    if (!extras.pprs_json.empty()) {
        out.patch_u32(reserved_pos + 24, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("PPRS");
        out.u8(1);
        out.str32(extras.pprs_json);
    }

    // ── XPLG (v14: external plugin state) ────────────────────────────
    if (!extras.external.empty()) {
        out.patch_u32(reserved_pos + 28, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("XPLG");
        out.u16(static_cast<std::uint16_t>(extras.external.size()));
        for (const FtrkExternalPlugin& external : extras.external) {
            out.str16(external.workspace_id);
            out.u8(external.kind);
            out.str16(external.plugin_id);
            out.str16(external.library_path);
            out.u32(static_cast<std::uint32_t>(external.state.size()));
            out.blob(external.state);
            out.u16(static_cast<std::uint16_t>(external.params.size()));
            for (const auto& [id, value] : external.params) {
                out.u32(id);
                out.f64(value);
            }
        }
    }

    // ── XINS (v15: per-instance NTP state) ───────────────────────────
    if (!extras.instance_states.empty()) {
        out.patch_u32(reserved_pos + 32, static_cast<std::uint32_t>(out.bytes.size()));
        out.magic("XINS");
        out.u8(1); // block version
        out.u16(static_cast<std::uint16_t>(extras.instance_states.size()));
        for (const FtrkInstanceState& state : extras.instance_states) {
            out.str16(state.workspace_id);
            out.u16(static_cast<std::uint16_t>(state.env_stages.size()));
            for (const FtrkInstanceEnvStage& env : state.env_stages) {
                out.str16(env.node_id);
                out.u8(static_cast<std::uint8_t>(env.stage_index));
                out.f64(env.target);
                out.f64(env.time);
            }
            out.u16(static_cast<std::uint16_t>(state.stage_chunks.size()));
            for (const FtrkInstanceStageChunk& chunk : state.stage_chunks) {
                out.str16(chunk.node_id);
                out.u32(static_cast<std::uint32_t>(chunk.chunk.size()));
                out.blob(chunk.chunk);
            }
        }
    }

    return std::move(out.bytes);
}

bool write_ftrk_file(const std::filesystem::path& path, const engine::TrackerProject& project,
                     const FtrkWriteExtras& extras, std::string& error) {
    const std::vector<std::uint8_t> bytes = write_ftrk(project, extras);
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string() + " for writing";
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/file seam
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file.good()) {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

} // namespace nt::io
