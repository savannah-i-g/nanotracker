#include "io/ftrk_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace nt::io {

namespace {

// Bounds-checked little-endian cursor. Every read throws
// TruncatedError past the end; core parsing catches it and fails the
// load, block parsers catch it and skip the block.
struct TruncatedError {};

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    void seek(std::size_t offset) {
        if (offset > size_) {
            throw TruncatedError{};
        }
        pos_ = offset;
    }

    [[nodiscard]] std::size_t pos() const { return pos_; }

    [[nodiscard]] std::size_t size() const { return size_; }

    std::uint8_t u8() {
        need(1);
        return data_[pos_++];
    }

    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }

    std::uint16_t u16() {
        need(2);
        const std::uint16_t v =
            static_cast<std::uint16_t>(data_[pos_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[pos_ + 1]) << 8U);
        pos_ += 2;
        return v;
    }

    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        std::memcpy(&v, data_ + pos_, 4); // little-endian host assumption documented in 02
        pos_ += 4;
        return v;
    }

    float f32() {
        need(4);
        float v = 0;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }

    // Little-endian f64 = low u32 then high u32 (the writer's f64
    // layout; the XPLG parser open-codes the same split).
    double f64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        const std::uint64_t raw = lo | (hi << 32U);
        double v = 0.0;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }

    // Fixed-size zero-terminated field (name fields).
    std::string fixed_string(std::size_t len) {
        need(len);
        // Byte-buffer → text reinterpretation is the deserializer's job.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const char* start = reinterpret_cast<const char*>(data_ + pos_);
        std::size_t end = 0;
        while (end < len && start[end] != '\0') {
            ++end;
        }
        pos_ += len;
        return {start, end};
    }

    // Length-delimited UTF-8 string.
    std::string bytes_string(std::size_t len) {
        need(len);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const char* start = reinterpret_cast<const char*>(data_ + pos_);
        pos_ += len;
        return {start, len};
    }

    std::vector<std::uint8_t> bytes(std::size_t len) {
        need(len);
        std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + len);
        pos_ += len;
        return out;
    }

    void skip(std::size_t len) {
        need(len);
        pos_ += len;
    }

private:
    void need(std::size_t n) const {
        if (pos_ + n > size_) {
            throw TruncatedError{};
        }
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

bool magic_is(Cursor& c, const char* magic) {
    return c.fixed_string(4) == magic;
}

void parse_fx_block(Cursor c, std::size_t offset, int pattern_count, int rows_per_pattern,
                    engine::TrackerProject& project, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "FXMX")) {
        extras.warnings.emplace_back("FXMX: bad magic, block skipped");
        return;
    }
    const int channel_count = c.u8();
    for (int i = 0; i < channel_count; ++i) {
        FtrkFxChannel ch;
        ch.name = c.fixed_string(16);
        ch.volume = c.u8();
        // Stored as int8 (-100..100 pan law).
        ch.pan = static_cast<std::int8_t>(c.u8()); // NOLINT(bugprone-signed-char-misuse)
        ch.enabled = c.u8() != 0;
        ch.color = c.bytes_string(c.u8());
        const int sends = c.u8();
        for (int s = 0; s < sends; ++s) {
            ch.tracker_sends.push_back(c.f32());
        }
        const int modules = c.u8();
        for (int m = 0; m < modules; ++m) {
            FtrkFxModule mod;
            mod.module_id = c.fixed_string(16);
            mod.enabled = c.u8() != 0;
            const int params = c.u8();
            for (int pi = 0; pi < params; ++pi) {
                const std::string key = c.bytes_string(c.u8());
                mod.params.emplace_back(key, c.f32());
            }
            ch.modules.push_back(std::move(mod));
        }
        extras.fx_channels.push_back(std::move(ch));
    }

    const int fx_pattern_count = c.u8();
    for (int pi = 0; pi < fx_pattern_count; ++pi) {
        engine::FxPattern fx;
        const int row_count = c.u16();
        for (int r = 0; r < row_count; ++r) {
            if (c.u8() == 0) {
                fx.present.push_back(false);
                fx.cells.emplace_back();
            } else {
                engine::FxCell cell;
                cell.channel_index = c.u8();
                cell.module_index = c.u8();
                cell.param_key = c.bytes_string(c.u8());
                cell.value = c.f32();
                fx.present.push_back(true);
                fx.cells.push_back(std::move(cell));
            }
        }
        project.fx_mixer.fx_patterns.push_back(std::move(fx));
    }
    // Pad to pattern count like the web reader.
    while (static_cast<int>(project.fx_mixer.fx_patterns.size()) < pattern_count) {
        engine::FxPattern fx;
        fx.present.assign(static_cast<std::size_t>(rows_per_pattern), false);
        fx.cells.resize(static_cast<std::size_t>(rows_per_pattern));
        project.fx_mixer.fx_patterns.push_back(std::move(fx));
    }
}

void parse_ins_table(Cursor c, std::size_t offset, int version, engine::TrackerProject& project,
                     FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "INTB")) {
        extras.warnings.emplace_back("INTB: bad magic, block skipped");
        return;
    }
    const int entry_count = c.u8();
    for (int i = 0; i < entry_count; ++i) {
        engine::InstrumentTableEntry entry;
        const int type = c.u8();
        if (type == 0) {
            entry.type = engine::InstrumentSourceType::kSample;
            entry.sample_id = c.u8();
        } else if (type == 1) {
            entry.type = engine::InstrumentSourceType::kPlugin;
            entry.plugin_id = c.bytes_string(c.u8());
            const int params = c.u8();
            for (int pi = 0; pi < params; ++pi) {
                const std::string key = c.bytes_string(c.u8());
                entry.plugin_preset_params.emplace_back(key, c.f32());
            }
        } else if (type == 2) {
            entry.type = engine::InstrumentSourceType::kWorkspace;
            entry.workspace_id = c.bytes_string(c.u16());
        } else {
            extras.warnings.emplace_back("INTB: unknown entry type " + std::to_string(type));
            c.skip(1); // mirror the web reader's best-effort resync
        }
        project.instrument_table.push_back(std::move(entry));
    }

    if (version >= 9) {
        // BNDT trailer: per-entry bound tracks.
        if (magic_is(c, "BNDT")) {
            for (int i = 0; i < entry_count; ++i) {
                const int count = c.u8();
                std::vector<int> tracks;
                tracks.reserve(static_cast<std::size_t>(count));
                for (int j = 0; j < count; ++j) {
                    tracks.push_back(c.u8());
                }
                // Sort + dedupe, matching the web loader's safety net.
                std::sort(tracks.begin(), tracks.end());
                tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
                project.instrument_table[static_cast<std::size_t>(i)].bound_tracks =
                    std::move(tracks);
            }
        }
    }
}

void parse_seq_block(Cursor c, std::size_t offset, engine::TrackerProject& project,
                     FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "SEQB")) {
        extras.warnings.emplace_back("SEQB: bad magic, block skipped");
        return;
    }
    const int pattern_count = c.u16();
    const int channel_count = c.u8();
    for (int pi = 0; pi < pattern_count; ++pi) {
        engine::SequencePattern sp;
        for (int ch = 0; ch < channel_count; ++ch) {
            std::vector<engine::SequenceLayer> layers;
            const int layer_count = c.u8();
            for (int li = 0; li < layer_count; ++li) {
                engine::SequenceLayer layer;
                layer.instrument = c.u8();
                layer.enabled = c.u8() != 0;
                const int notes = c.u16();
                for (int ni = 0; ni < notes; ++ni) {
                    engine::SequenceNote note;
                    note.pitch = c.u8();
                    note.start_tick = c.u16();
                    note.duration_ticks = c.u16();
                    note.velocity = c.u8();
                    layer.notes.push_back(note);
                }
                layers.push_back(std::move(layer));
            }
            sp.layers.push_back(std::move(layers));
        }
        project.sequence_mixer.seq_patterns.push_back(std::move(sp));
    }

    // Sparse custom channel colors (alpha bit marks "set").
    if (c.pos() < c.size()) {
        const int color_count = c.u8();
        for (int i = 0; i < color_count; ++i) {
            const int ch = c.u8();
            const std::uint32_t r = c.u8();
            const std::uint32_t g = c.u8();
            const std::uint32_t b = c.u8();
            if (ch < engine::kMaxChannels) {
                project.channel_colors[static_cast<std::size_t>(ch)] =
                    0xFF000000U | (r << 16U) | (g << 8U) | b;
            }
        }
    }
}

void parse_wpbr(Cursor c, std::size_t offset, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "WPBR")) {
        extras.warnings.emplace_back("WPBR: bad magic, block skipped");
        return;
    }
    extras.workspace_json = c.bytes_string(c.u32());
}

void parse_plgb(Cursor c, std::size_t offset, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "PLGB")) {
        extras.warnings.emplace_back("PLGB: bad magic, block skipped");
        return;
    }
    const int count = c.u16();
    for (int i = 0; i < count; ++i) {
        FtrkBundledPlugin plugin;
        plugin.plugin_id = c.bytes_string(c.u16());
        plugin.archive = c.bytes(c.u32());
        extras.plugins.push_back(std::move(plugin));
    }
}

// POVR block version 1 (web trackerSerializer.ts:432): per instance
// { iid str16, slotCount u16, per slot { slotId str16, hash str16,
// name str16, sampleRate u32, channels u8, duration f32, byteLen u32
// + bytes } }. Each unique hash's bytes appear once; later references
// carry byteLen 0 and re-use the first occurrence. Anything this
// reader cannot interpret — unknown block version, a forward
// reference to a hash never carried — keeps the whole block verbatim
// in povr_raw so the writer round-trips it untouched. `end_bound` is
// where the next block (or the file) ends: the block is not
// self-delimiting from the header alone.
void parse_povr(Cursor c, std::size_t offset, std::size_t end_bound, FtrkExtras& extras) {
    const auto keep_raw = [&](const char* why) {
        Cursor raw(c);
        raw.seek(offset);
        extras.povr_raw = raw.bytes(end_bound - offset);
        extras.warnings.emplace_back(std::string("POVR: ") + why + ", block carried verbatim");
    };

    c.seek(offset);
    if (!magic_is(c, "POVR")) {
        extras.warnings.emplace_back("POVR: bad magic, block skipped");
        return;
    }
    if (c.u8() != 1) {
        keep_raw("unknown block version");
        return;
    }
    // Parsed locally and committed whole: a truncation mid-block must
    // not leave half the overrides adopted (the tolerant() wrapper
    // only reports, it cannot roll back).
    std::vector<FtrkPovrOverride> parsed;
    std::vector<std::pair<std::string, std::size_t>> bytes_by_hash; // hash → entry index
    const int instance_count = c.u16();
    for (int i = 0; i < instance_count; ++i) {
        const std::string instance_id = c.bytes_string(c.u16());
        const int slot_count = c.u16();
        for (int s = 0; s < slot_count; ++s) {
            FtrkPovrOverride entry;
            entry.instance_id = instance_id;
            entry.slot_id = c.bytes_string(c.u16());
            entry.hash = c.bytes_string(c.u16());
            entry.name = c.bytes_string(c.u16());
            entry.sample_rate = c.u32();
            entry.channels = c.u8();
            entry.duration = c.f32();
            const std::uint32_t byte_len = c.u32();
            if (byte_len > 0) {
                entry.bytes = c.bytes(byte_len);
                bytes_by_hash.emplace_back(entry.hash, parsed.size());
            } else {
                const auto it =
                    std::find_if(bytes_by_hash.begin(), bytes_by_hash.end(),
                                 [&entry](const auto& seen) { return seen.first == entry.hash; });
                if (it == bytes_by_hash.end()) {
                    keep_raw("dedup reference to a hash never carried");
                    return;
                }
                entry.bytes = parsed[it->second].bytes;
            }
            parsed.push_back(std::move(entry));
        }
    }
    extras.povr_overrides = std::move(parsed);
}

void parse_pprs(Cursor c, std::size_t offset, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "PPRS")) {
        extras.warnings.emplace_back("PPRS: bad magic, block skipped");
        return;
    }
    if (c.u8() != 1) {
        extras.warnings.emplace_back("PPRS: unsupported block version, skipped");
        return;
    }
    extras.pprs_json = c.bytes_string(c.u32());
}

void parse_xplg(Cursor c, std::size_t offset, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "XPLG")) {
        extras.warnings.emplace_back("XPLG: bad magic, block skipped");
        return;
    }
    const int count = c.u16();
    for (int i = 0; i < count; ++i) {
        FtrkExternalPlugin external;
        external.workspace_id = c.bytes_string(c.u16());
        external.kind = c.u8();
        external.plugin_id = c.bytes_string(c.u16());
        external.library_path = c.bytes_string(c.u16());
        external.state = c.bytes(c.u32());
        const int params = c.u16();
        for (int p = 0; p < params; ++p) {
            const std::uint32_t id = c.u32();
            const std::uint64_t lo = c.u32();
            const std::uint64_t hi = c.u32();
            const std::uint64_t raw = lo | (hi << 32);
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            external.params.emplace_back(id, value);
        }
        extras.external.push_back(std::move(external));
    }
}

// XINS block version 1 (v15, native): count u16; per instance
// { workspaceId str16, envStageCount u16 × { nodeId str16, stageIndex
// u8, target f64, time f64 }, stageChunkCount u16 × { nodeId str16,
// chunkLen u32 + bytes } }. Parsed whole then committed, so a
// truncation mid-block adopts nothing.
void parse_xins(Cursor c, std::size_t offset, FtrkExtras& extras) {
    c.seek(offset);
    if (!magic_is(c, "XINS")) {
        extras.warnings.emplace_back("XINS: bad magic, block skipped");
        return;
    }
    if (c.u8() != 1) {
        extras.warnings.emplace_back("XINS: unsupported block version, skipped");
        return;
    }
    std::vector<FtrkInstanceState> parsed;
    const int count = c.u16();
    for (int i = 0; i < count; ++i) {
        FtrkInstanceState state;
        state.workspace_id = c.bytes_string(c.u16());
        const int env_count = c.u16();
        for (int e = 0; e < env_count; ++e) {
            FtrkInstanceEnvStage env;
            env.node_id = c.bytes_string(c.u16());
            env.stage_index = c.u8();
            env.target = c.f64();
            env.time = c.f64();
            state.env_stages.push_back(std::move(env));
        }
        const int chunk_count = c.u16();
        for (int ci = 0; ci < chunk_count; ++ci) {
            FtrkInstanceStageChunk chunk;
            chunk.node_id = c.bytes_string(c.u16());
            chunk.chunk = c.bytes(c.u32());
            state.stage_chunks.push_back(std::move(chunk));
        }
        parsed.push_back(std::move(state));
    }
    extras.instance_states = std::move(parsed);
}

template <typename Fn>
void tolerant(FtrkExtras& extras, const char* block, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const TruncatedError&) {
        extras.warnings.emplace_back(std::string(block) + ": truncated, block skipped");
    }
}

} // namespace

std::optional<FtrkReadResult> read_ftrk(const std::uint8_t* data, std::size_t size,
                                        std::string& error) {
    FtrkReadResult result;
    engine::TrackerProject& project = result.project;
    FtrkExtras& extras = result.extras;

    try {
        Cursor c(data, size);
        if (!magic_is(c, "FTRK")) {
            error = "not a .ftrk file (bad magic)";
            return std::nullopt;
        }
        const int version = c.u16();
        if (version < 1 || version > 15) {
            error = "unsupported .ftrk version " + std::to_string(version) + " (supported: 1-15)";
            return std::nullopt;
        }
        extras.version = version;

        project.name = c.fixed_string(32);
        project.bpm = version >= 2 ? static_cast<int>(c.f32()) : c.u16();
        project.speed = c.u8();
        project.rows_per_pattern = c.u8();
        project.channels = c.u8();
        const int pattern_count = c.u8();
        const int order_length = c.u16();
        const int sample_count = c.u8();

        const std::size_t reserved_pos = c.pos();
        const std::uint32_t fx_offset = version >= 4 ? (c.seek(reserved_pos), c.u32()) : 0;
        const std::uint32_t ins_offset = version >= 5 ? (c.seek(reserved_pos + 4), c.u32()) : 0;
        const std::uint32_t wpbr_offset = version >= 7 ? (c.seek(reserved_pos + 8), c.u32()) : 0;
        const std::uint32_t plgb_offset = version >= 8 ? (c.seek(reserved_pos + 12), c.u32()) : 0;
        const std::uint32_t seq_offset = version >= 10 ? (c.seek(reserved_pos + 16), c.u32()) : 0;
        const std::uint32_t povr_offset = version >= 12 ? (c.seek(reserved_pos + 20), c.u32()) : 0;
        const std::uint32_t pprs_offset = version >= 13 ? (c.seek(reserved_pos + 24), c.u32()) : 0;
        const std::uint32_t xplg_offset = version >= 14 ? (c.seek(reserved_pos + 28), c.u32()) : 0;
        const std::uint32_t xins_offset = version >= 15 ? (c.seek(reserved_pos + 32), c.u32()) : 0;
        // Reserved region: 39 bytes from v15 (ninth offset, XINS), 35 in
        // v1 and v14 (XPLG), 31 across v2-13.
        std::size_t reserved_len = 35; // v1 and v14
        if (version >= 15) {
            reserved_len = 39;
        } else if (version >= 2 && version < 14) {
            reserved_len = 31;
        }
        c.seek(reserved_pos + reserved_len);

        for (int i = 0; i < order_length; ++i) {
            project.order_list.push_back(c.u8());
        }

        for (int p = 0; p < pattern_count; ++p) {
            engine::TrackerPattern pattern;
            pattern.id = p;
            pattern.name = c.fixed_string(16);
            int rows = project.rows_per_pattern;
            if (version >= 6) {
                rows = c.u16();
                pattern.highlight_major = c.u8();
                pattern.highlight_minor = c.u8();
            }
            pattern.rows.reserve(static_cast<std::size_t>(rows));
            for (int r = 0; r < rows; ++r) {
                std::vector<engine::TrackerCell> row;
                row.reserve(static_cast<std::size_t>(project.channels));
                for (int ch = 0; ch < project.channels; ++ch) {
                    engine::TrackerCell cell;
                    cell.note = c.u8();
                    const std::uint8_t raw_ins = c.u8();
                    // v9+: slot in the low 5 bits, bound sub-slot in
                    // the top 3; earlier files use the whole byte.
                    cell.instrument = version >= 9 ? raw_ins & 0x1FU : raw_ins;
                    cell.bound_index = version >= 9 ? (raw_ins >> 5U) & 0x07U : 0;
                    cell.volume = c.u8();
                    cell.effect = c.u8();
                    cell.effect_param = c.u8();
                    row.push_back(cell);
                }
                pattern.rows.push_back(std::move(row));
            }
            project.patterns.push_back(std::move(pattern));
        }

        struct PendingSample {
            engine::TrackerSample meta;
            std::uint32_t data_length = 0;
        };

        std::vector<PendingSample> pending;
        for (int i = 0; i < sample_count; ++i) {
            PendingSample ps;
            ps.meta.id = c.u8();
            const int fmt = c.u8();
            if (fmt == 1) {
                ps.meta.format = "ogg";
            } else if (fmt == 2) {
                ps.meta.format = "mp3";
            } else {
                ps.meta.format = "wav";
            }
            ps.meta.base_note = c.u8();
            // Finetune is a signed byte by format definition.
            ps.meta.finetune = c.i8(); // NOLINT(bugprone-signed-char-misuse)
            ps.meta.volume = c.u8();
            ps.meta.pan = c.u8();
            ps.meta.loop_start = c.u32();
            ps.meta.loop_length = c.u32();
            ps.meta.sample_rate = c.u32();
            ps.meta.num_channels = c.u8();
            ps.meta.frames = c.u32();
            ps.data_length = c.u32();
            ps.meta.name = c.fixed_string(22);
            ps.meta.file_name = c.fixed_string(32);
            const std::uint16_t stretch = c.u16();
            ps.meta.stretch_ratio = (version >= 2 && stretch > 0) ? stretch / 10000.0 : 1.0;
            ps.meta.category = version >= 3 ? c.u8() : 0;
            pending.push_back(std::move(ps));
        }
        for (PendingSample& ps : pending) {
            ps.meta.original_data = c.bytes(ps.data_length);
            project.samples.push_back(std::move(ps.meta));
        }

        const auto in_range = [size](std::uint32_t off) { return off > 0 && off < size; };
        if (in_range(fx_offset)) {
            tolerant(extras, "FXMX", [&] {
                parse_fx_block(Cursor(data, size), fx_offset, pattern_count,
                               project.rows_per_pattern, project, extras);
            });
        }
        if (in_range(ins_offset)) {
            tolerant(extras, "INTB", [&] {
                parse_ins_table(Cursor(data, size), ins_offset, version, project, extras);
            });
        }
        if (in_range(wpbr_offset)) {
            tolerant(extras, "WPBR", [&] { parse_wpbr(Cursor(data, size), wpbr_offset, extras); });
        }
        if (in_range(plgb_offset)) {
            tolerant(extras, "PLGB", [&] { parse_plgb(Cursor(data, size), plgb_offset, extras); });
        }
        if (in_range(seq_offset)) {
            tolerant(extras, "SEQB",
                     [&] { parse_seq_block(Cursor(data, size), seq_offset, project, extras); });
        }
        if (in_range(povr_offset)) {
            tolerant(extras, "POVR", [&] {
                // POVR is not self-delimiting, so a verbatim carry
                // needs the block's end: the nearest following block
                // offset, else end of file. (The old to-EOF grab
                // swallowed PPRS/XPLG into the raw copy.)
                std::size_t end_bound = size;
                for (const std::uint32_t other : {pprs_offset, xplg_offset}) {
                    if (other > povr_offset && other < end_bound) {
                        end_bound = other;
                    }
                }
                parse_povr(Cursor(data, size), povr_offset, end_bound, extras);
            });
        }
        if (in_range(pprs_offset)) {
            tolerant(extras, "PPRS", [&] { parse_pprs(Cursor(data, size), pprs_offset, extras); });
        }
        if (in_range(xplg_offset)) {
            tolerant(extras, "XPLG", [&] { parse_xplg(Cursor(data, size), xplg_offset, extras); });
        }
        if (in_range(xins_offset)) {
            tolerant(extras, "XINS", [&] { parse_xins(Cursor(data, size), xins_offset, extras); });
        }
    } catch (const TruncatedError&) {
        error = "truncated .ftrk file";
        return std::nullopt;
    }

    return result;
}

std::optional<FtrkReadResult> read_ftrk_file(const std::filesystem::path& path,
                                             std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    return read_ftrk(data.data(), data.size(), error);
}

} // namespace nt::io
