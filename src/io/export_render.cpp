#include "io/export_render.h"

#include "app/project_session.h"
#include "audio/audio_engine.h"
#include "platform/shared_library.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <miniz.h>
#include <vorbis/vorbisenc.h>

namespace nt::io {

// Codec/file interop: WAV headers, ogg_page buffers and stream writes
// all cross C APIs through byte pointers — reinterpret_cast is the
// honest spelling at those seams.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

namespace {

// ── WAV ──────────────────────────────────────────────────────────────

// RIFF LIST/INFO metadata chunk (complete "LIST" chunk bytes; empty
// when no field is set). Sub-chunk text is null-terminated and padded
// to even length per RIFF rules.
std::vector<std::uint8_t> build_info_chunk(const ExportMetadata& meta) {
    std::vector<std::pair<const char*, const std::string*>> fields;
    const std::string encoder = "nanoTracker";
    if (!meta.title.empty()) {
        fields.emplace_back("INAM", &meta.title);
    }
    if (!meta.artist.empty()) {
        fields.emplace_back("IART", &meta.artist);
    }
    if (!meta.album.empty()) {
        fields.emplace_back("IPRD", &meta.album);
    }
    if (!meta.date.empty()) {
        fields.emplace_back("ICRD", &meta.date);
    }
    if (!meta.comment.empty()) {
        fields.emplace_back("ICMT", &meta.comment);
    }
    if (fields.empty()) {
        return {};
    }
    fields.emplace_back("ISFT", &encoder);

    std::vector<std::uint8_t> chunk;
    auto push_u32 = [&chunk](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) {
            chunk.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF));
        }
    };
    auto push_id = [&chunk](const char* id) { chunk.insert(chunk.end(), id, id + 4); };
    push_id("LIST");
    push_u32(0); // payload size backfilled below
    push_id("INFO");
    for (const auto& [id, text] : fields) {
        const auto text_bytes = static_cast<std::uint32_t>(text->size() + 1); // incl. NUL
        push_id(id);
        push_u32(text_bytes);
        chunk.insert(chunk.end(), text->begin(), text->end());
        chunk.push_back(0);
        if ((text_bytes & 1U) != 0U) {
            chunk.push_back(0); // pad to even
        }
    }
    const auto payload = static_cast<std::uint32_t>(chunk.size() - 8);
    for (int b = 0; b < 4; ++b) {
        chunk[4 + static_cast<std::size_t>(b)] =
            static_cast<std::uint8_t>((payload >> (8 * b)) & 0xFF);
    }
    return chunk;
}

bool write_wav(const std::filesystem::path& path, const std::vector<float>& interleaved,
               std::uint32_t rate, WavDepth depth, const ExportMetadata& meta, std::string& error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    const bool is_float = depth == WavDepth::kFloat32;
    std::uint16_t bits = 16;
    if (depth == WavDepth::kPcm24) {
        bits = 24;
    } else if (is_float) {
        bits = 32;
    }
    const auto block_align = static_cast<std::uint16_t>(2 * bits / 8);
    const auto frames = static_cast<std::uint32_t>(interleaved.size() / 2);
    const std::uint32_t data_bytes = frames * block_align;
    const std::vector<std::uint8_t> info = build_info_chunk(meta);

    // Non-PCM (float) gets the extended 18-byte fmt (cbSize = 0) plus a
    // fact chunk, per the WAVE spec; PCM keeps the classic 16-byte fmt.
    const std::uint32_t fmt_size = is_float ? 18 : 16;
    const std::uint32_t riff_size = 4 + (8 + fmt_size) + (is_float ? 8 + 4 : 0) + (8 + data_bytes) +
                                    static_cast<std::uint32_t>(info.size());

    auto u32 = [&file](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&file](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(riff_size);
    file.write("WAVEfmt ", 8);
    u32(fmt_size);
    u16(is_float ? 3 : 1); // format tag: IEEE float = 3, PCM = 1
    u16(2);
    u32(rate);
    u32(rate * block_align);
    u16(block_align);
    u16(bits);
    if (is_float) {
        u16(0); // cbSize
        file.write("fact", 4);
        u32(4);
        u32(frames);
    }
    file.write("data", 4);
    u32(data_bytes);
    switch (depth) {
    case WavDepth::kPcm16:
        for (const float sample : interleaved) {
            const auto v = static_cast<std::int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
            u16(static_cast<std::uint16_t>(v));
        }
        break;
    case WavDepth::kPcm24:
        for (const float sample : interleaved) {
            const auto v = static_cast<std::int32_t>(std::clamp(sample, -1.0F, 1.0F) * 8388607.0F);
            const auto u = static_cast<std::uint32_t>(v);
            const std::array<char, 3> bytes{static_cast<char>(u & 0xFF),
                                            static_cast<char>((u >> 8) & 0xFF),
                                            static_cast<char>((u >> 16) & 0xFF)};
            file.write(bytes.data(), 3);
        }
        break;
    case WavDepth::kFloat32:
        // Float carries over-full-scale honestly; no clamp.
        file.write(reinterpret_cast<const char*>(interleaved.data()),
                   static_cast<std::streamsize>(interleaved.size() * sizeof(float)));
        break;
    }
    if (!info.empty()) {
        file.write(reinterpret_cast<const char*>(info.data()),
                   static_cast<std::streamsize>(info.size()));
    }
    if (!file.good()) {
        error = "write failed";
        return false;
    }
    return true;
}

// ── OGG (libvorbis) ──────────────────────────────────────────────────

bool write_ogg(const std::filesystem::path& path, const std::vector<float>& interleaved,
               std::uint32_t rate, float quality, const ExportMetadata& meta, std::string& error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 2, static_cast<long>(rate),
                               std::clamp(quality, -0.1F, 1.0F)) != 0) {
        vorbis_info_clear(&info);
        error = "vorbis encoder init failed";
        return false;
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_comment_add_tag(&comment, "ENCODER", "nanoTracker");
    auto add_tag = [&comment](const char* key, const std::string& value) {
        if (!value.empty()) {
            vorbis_comment_add_tag(&comment, key, value.c_str());
        }
    };
    add_tag("TITLE", meta.title);
    add_tag("ARTIST", meta.artist);
    add_tag("ALBUM", meta.album);
    add_tag("DATE", meta.date);
    add_tag("COMMENT", meta.comment);
    vorbis_dsp_state dsp;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block block;
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream;
    ogg_stream_init(&stream, 1);

    ogg_packet header;
    ogg_packet header_comment;
    ogg_packet header_code;
    vorbis_analysis_headerout(&dsp, &comment, &header, &header_comment, &header_code);
    ogg_stream_packetin(&stream, &header);
    ogg_stream_packetin(&stream, &header_comment);
    ogg_stream_packetin(&stream, &header_code);
    ogg_page page;
    while (ogg_stream_flush(&stream, &page) != 0) {
        file.write(reinterpret_cast<const char*>(page.header), page.header_len);
        file.write(reinterpret_cast<const char*>(page.body), page.body_len);
    }

    const auto frames = static_cast<long>(interleaved.size() / 2);
    long done = 0;
    bool end_of_stream = false;
    while (!end_of_stream) {
        const long chunk = std::min<long>(1024, frames - done);
        if (chunk <= 0) {
            vorbis_analysis_wrote(&dsp, 0); // end of input
        } else {
            float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(chunk));
            for (long i = 0; i < chunk; ++i) {
                buffer[0][i] = interleaved[static_cast<std::size_t>(done + i) * 2];
                buffer[1][i] = interleaved[(static_cast<std::size_t>(done + i) * 2) + 1];
            }
            vorbis_analysis_wrote(&dsp, static_cast<int>(chunk));
            done += chunk;
        }
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp, &packet) == 1) {
                ogg_stream_packetin(&stream, &packet);
                while (ogg_stream_pageout(&stream, &page) != 0) {
                    file.write(reinterpret_cast<const char*>(page.header), page.header_len);
                    file.write(reinterpret_cast<const char*>(page.body), page.body_len);
                    if (ogg_page_eos(&page) != 0) {
                        end_of_stream = true;
                    }
                }
            }
        }
        if (chunk <= 0 && !end_of_stream) {
            while (ogg_stream_flush(&stream, &page) != 0) {
                file.write(reinterpret_cast<const char*>(page.header), page.header_len);
                file.write(reinterpret_cast<const char*>(page.body), page.body_len);
            }
            end_of_stream = true;
        }
    }
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    if (!file.good()) {
        error = "write failed";
        return false;
    }
    return true;
}

// ── MP3 (runtime-loaded system libmp3lame — LGPL dynamic linking) ───

struct LameApi {
    void* handle = nullptr;
    void* (*init)() = nullptr;
    int (*set_in_samplerate)(void*, int) = nullptr;
    int (*set_num_channels)(void*, int) = nullptr;
    int (*set_brate)(void*, int) = nullptr;
    int (*set_quality)(void*, int) = nullptr;
    int (*init_params)(void*) = nullptr;
    int (*encode_interleaved)(void*, short*, int, unsigned char*, int) = nullptr;
    int (*encode_flush)(void*, unsigned char*, int) = nullptr;
    int (*close)(void*) = nullptr;
    // id3tag_* are optional: absent symbols mean MP3 tags are silently
    // skipped (old or trimmed lame builds).
    void (*id3_init)(void*) = nullptr;
    void (*id3_add_v2)(void*) = nullptr;
    void (*id3_set_title)(void*, const char*) = nullptr;
    void (*id3_set_artist)(void*, const char*) = nullptr;
    void (*id3_set_album)(void*, const char*) = nullptr;
    void (*id3_set_year)(void*, const char*) = nullptr;
    void (*id3_set_comment)(void*, const char*) = nullptr;
};

bool load_lame(LameApi& api, std::string& error) {
#ifdef _WIN32
    constexpr std::array<const char*, 2> kNames{"libmp3lame.dll", "lame_enc.dll"};
#else
    constexpr std::array<const char*, 2> kNames{"libmp3lame.so.0", "libmp3lame.so"};
#endif
    std::string open_error;
    for (const char* name : kNames) {
        api.handle = platform::library_open(name, open_error);
        if (api.handle != nullptr) {
            break;
        }
    }
    if (api.handle == nullptr) {
        error = "libmp3lame not found — install it (package libmp3lame0 / libmp3lame.dll beside "
                "the executable) for MP3 export";
        return false;
    }
    auto resolve = [&api](const char* name) { return platform::library_symbol(api.handle, name); };
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) — dlsym contract
    api.init = reinterpret_cast<void* (*)()>(resolve("lame_init"));
    api.set_in_samplerate =
        reinterpret_cast<int (*)(void*, int)>(resolve("lame_set_in_samplerate"));
    api.set_num_channels = reinterpret_cast<int (*)(void*, int)>(resolve("lame_set_num_channels"));
    api.set_brate = reinterpret_cast<int (*)(void*, int)>(resolve("lame_set_brate"));
    api.set_quality = reinterpret_cast<int (*)(void*, int)>(resolve("lame_set_quality"));
    api.init_params = reinterpret_cast<int (*)(void*)>(resolve("lame_init_params"));
    api.encode_interleaved = reinterpret_cast<int (*)(void*, short*, int, unsigned char*, int)>(
        resolve("lame_encode_buffer_interleaved"));
    api.encode_flush =
        reinterpret_cast<int (*)(void*, unsigned char*, int)>(resolve("lame_encode_flush"));
    api.close = reinterpret_cast<int (*)(void*)>(resolve("lame_close"));
    api.id3_init = reinterpret_cast<void (*)(void*)>(resolve("id3tag_init"));
    api.id3_add_v2 = reinterpret_cast<void (*)(void*)>(resolve("id3tag_add_v2"));
    api.id3_set_title = reinterpret_cast<void (*)(void*, const char*)>(resolve("id3tag_set_title"));
    api.id3_set_artist =
        reinterpret_cast<void (*)(void*, const char*)>(resolve("id3tag_set_artist"));
    api.id3_set_album = reinterpret_cast<void (*)(void*, const char*)>(resolve("id3tag_set_album"));
    api.id3_set_year = reinterpret_cast<void (*)(void*, const char*)>(resolve("id3tag_set_year"));
    api.id3_set_comment =
        reinterpret_cast<void (*)(void*, const char*)>(resolve("id3tag_set_comment"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (api.init == nullptr || api.init_params == nullptr || api.encode_interleaved == nullptr ||
        api.encode_flush == nullptr) {
        error = "libmp3lame is present but missing expected symbols";
        platform::library_close(api.handle);
        api.handle = nullptr;
        return false;
    }
    return true;
}

bool write_mp3(const std::filesystem::path& path, const std::vector<float>& interleaved,
               std::uint32_t rate, int bitrate_kbps, int quality, const ExportMetadata& meta,
               std::string& error) {
    LameApi api;
    if (!load_lame(api, error)) {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        platform::library_close(api.handle);
        return false;
    }
    void* lame = api.init();
    if (api.set_in_samplerate != nullptr) {
        api.set_in_samplerate(lame, static_cast<int>(rate));
    }
    if (api.set_num_channels != nullptr) {
        api.set_num_channels(lame, 2);
    }
    if (api.set_brate != nullptr) {
        api.set_brate(lame, std::clamp(bitrate_kbps, 8, 320));
    }
    if (api.set_quality != nullptr) {
        api.set_quality(lame, std::clamp(quality, 0, 9));
    }
    // Tags must precede init_params; lame emits the ID3v2 block at the
    // head of the stream on the first encode call.
    const bool has_tags = !meta.title.empty() || !meta.artist.empty() || !meta.album.empty() ||
                          !meta.date.empty() || !meta.comment.empty();
    if (has_tags && api.id3_init != nullptr) {
        api.id3_init(lame);
        if (api.id3_add_v2 != nullptr) {
            api.id3_add_v2(lame);
        }
        auto set_tag = [lame](void (*setter)(void*, const char*), const std::string& value) {
            if (setter != nullptr && !value.empty()) {
                setter(lame, value.c_str());
            }
        };
        set_tag(api.id3_set_title, meta.title);
        set_tag(api.id3_set_artist, meta.artist);
        set_tag(api.id3_set_album, meta.album);
        set_tag(api.id3_set_year, meta.date);
        set_tag(api.id3_set_comment, meta.comment);
    }
    if (api.init_params(lame) < 0) {
        error = "lame_init_params failed";
        api.close(lame);
        platform::library_close(api.handle);
        return false;
    }
    const auto frames = static_cast<long>(interleaved.size() / 2);
    std::vector<short> pcm(interleaved.size());
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        pcm[i] = static_cast<short>(std::clamp(interleaved[i], -1.0F, 1.0F) * 32767.0F);
    }
    std::vector<unsigned char> out(65536);
    long done = 0;
    while (done < frames) {
        const long chunk = std::min<long>(4608, frames - done);
        const int bytes = api.encode_interleaved(
            lame, pcm.data() + (static_cast<std::size_t>(done) * 2), static_cast<int>(chunk),
            out.data(), static_cast<int>(out.size()));
        if (bytes < 0) {
            error = "lame encode failed";
            api.close(lame);
            platform::library_close(api.handle);
            return false;
        }
        file.write(reinterpret_cast<const char*>(out.data()), bytes);
        done += chunk;
    }
    const int tail = api.encode_flush(lame, out.data(), static_cast<int>(out.size()));
    if (tail > 0) {
        file.write(reinterpret_cast<const char*>(out.data()), tail);
    }
    api.close(lame);
    platform::library_close(api.handle);
    if (!file.good()) {
        error = "write failed";
        return false;
    }
    return true;
}

// ── Render pass ──────────────────────────────────────────────────────

// One offline engine run over the staged FTRK bytes. `channel_mask`
// gates note triggers (stems); the transport is seeded at start_order
// and parks after end_order (audio_engine kTransportPlay contract).
// Pulls are one engine block so the parked transport is observed
// within a block of the final tick.
bool render_pass(const std::filesystem::path& staged_ftrk, std::uint32_t rate, double tail_seconds,
                 int start_order, int end_order, std::uint32_t channel_mask,
                 std::vector<float>& rendered, std::string& error) {
    audio::AudioEngine engine;
    if (!engine.start_offline(rate)) {
        error = "offline engine start failed";
        return false;
    }
    app::ProjectSession session(engine);
    if (!session.load_ftrk(staged_ftrk)) {
        error = "render load failed: " + session.error();
        return false;
    }
    engine.send(
        {.type = audio::Command::Type::kSetChannelMask, .aux_int = static_cast<int>(channel_mask)});
    engine.send({.type = audio::Command::Type::kTransportPlay,
                 .aux_int = start_order,
                 .aux_int2 = end_order + 1});

    const std::uint64_t max_frames = static_cast<std::uint64_t>(rate) * 600; // safety cap
    const auto tail_total = static_cast<std::uint64_t>(std::max(0.0, tail_seconds) * rate);
    std::array<float, static_cast<std::size_t>(audio::kBlockFrames) * 2> chunk{};
    bool ended = false;
    std::uint64_t tail_frames = 0;
    rendered.clear();
    while (rendered.size() / 2 < max_frames) {
        engine.render_offline(chunk.data(), audio::kBlockFrames);
        rendered.insert(rendered.end(), chunk.begin(), chunk.end());
        if (!ended && !engine.snapshot().transport_playing) {
            ended = true;
        }
        if (ended) {
            tail_frames += audio::kBlockFrames;
            if (tail_frames >= tail_total) {
                break;
            }
        }
    }
    session.stop();
    return true;
}

bool encode_buffer(const std::filesystem::path& path, const std::vector<float>& rendered,
                   std::uint32_t rate, const ExportOptions& options, std::string& error) {
    switch (options.format) {
    case ExportFormat::kWav:
        return write_wav(path, rendered, rate, options.wav_depth, options.metadata, error);
    case ExportFormat::kOgg:
        return write_ogg(path, rendered, rate, options.ogg_quality, options.metadata, error);
    case ExportFormat::kMp3:
        return write_mp3(path, rendered, rate, options.mp3_bitrate_kbps, options.mp3_quality,
                         options.metadata, error);
    }
    error = "unknown format";
    return false;
}

std::filesystem::path stem_path(const std::filesystem::path& base, int channel) {
    std::array<char, 8> suffix{};
    std::snprintf(suffix.data(), suffix.size(), ".ch%02d", channel + 1);
    std::filesystem::path out = base;
    out.replace_extension();
    out += suffix.data();
    out += base.extension();
    return out;
}

// Bundles the stem files into one ZIP (built in memory — all file I/O
// stays on std::filesystem paths) and removes the loose files.
bool zip_stems(const std::filesystem::path& zip_path,
               const std::vector<std::filesystem::path>& stems, std::string& error) {
    mz_zip_archive zip{};
    if (mz_zip_writer_init_heap(&zip, 0, 0) == MZ_FALSE) {
        error = "zip init failed";
        return false;
    }
    for (const std::filesystem::path& stem : stems) {
        std::ifstream in(stem, std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
        if (!in.good() && !in.eof()) {
            mz_zip_writer_end(&zip);
            error = "cannot read " + stem.string();
            return false;
        }
        if (mz_zip_writer_add_mem(&zip, stem.filename().string().c_str(), bytes.data(),
                                  bytes.size(), MZ_DEFAULT_LEVEL) == MZ_FALSE) {
            mz_zip_writer_end(&zip);
            error = "zip add failed for " + stem.filename().string();
            return false;
        }
    }
    void* buffer = nullptr;
    std::size_t size = 0;
    if (mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) == MZ_FALSE) {
        mz_zip_writer_end(&zip);
        error = "zip finalize failed";
        return false;
    }
    std::ofstream out(zip_path, std::ios::binary);
    out.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(size));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    if (!out.good()) {
        error = "cannot write " + zip_path.string();
        return false;
    }
    for (const std::filesystem::path& stem : stems) {
        std::error_code ec;
        std::filesystem::remove(stem, ec);
    }
    return true;
}

// Unique staging name: exports may run concurrently (tests, future
// worker thread) and must never share a temp file.
std::filesystem::path staging_path() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("nt_export_render_" + std::to_string(stamp) + "_" +
            std::to_string(counter.fetch_add(1)) + ".ftrk");
}

} // namespace

ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, const ExportOptions& options) {
    ExportResult result;

    const auto order_count = static_cast<int>(project.order_list.size());
    const int start = std::clamp(options.start_order, 0, std::max(0, order_count - 1));
    const int end = options.end_order < 0
                        ? std::max(start, order_count - 1)
                        : std::clamp(options.end_order, start, std::max(start, order_count - 1));
    const std::uint32_t rate = std::clamp<std::uint32_t>(options.sample_rate, 8000, 192000);

    // Serialise → temp file → fresh offline session per pass: every
    // pass renders exactly what a load of the saved project would play.
    const std::vector<std::uint8_t> bytes = write_ftrk(project, extras);
    const std::filesystem::path staged = staging_path();
    {
        std::ofstream staged_file(staged, std::ios::binary);
        staged_file.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
        if (!staged_file.good()) {
            result.error = "cannot stage temp project";
            return result;
        }
    }

    // Pass list: the full mix, or one gated pass per stem channel.
    struct Pass {
        std::uint32_t mask;
        std::filesystem::path out;
    };

    std::vector<Pass> passes;
    if (options.stem_mask == 0) {
        passes.push_back({~0U, path});
    } else {
        const int channels = std::min(project.channels, engine::kMaxChannels);
        for (int ch = 0; ch < channels; ++ch) {
            if ((options.stem_mask >> static_cast<unsigned>(ch) & 1U) != 0U) {
                passes.push_back({1U << static_cast<unsigned>(ch), stem_path(path, ch)});
            }
        }
        if (passes.empty()) {
            result.error = "stem mask matches no project channel";
            std::filesystem::remove(staged);
            return result;
        }
    }

    std::vector<float> rendered;
    for (const Pass& pass : passes) {
        if (!render_pass(staged, rate, options.tail_seconds, start, end, pass.mask, rendered,
                         result.error)) {
            std::filesystem::remove(staged);
            return result;
        }
        if (!apply_export_post(rendered, rate, options.post, result.error)) {
            std::filesystem::remove(staged);
            return result;
        }
        if (!encode_buffer(pass.out, rendered, rate, options, result.error)) {
            std::filesystem::remove(staged);
            return result;
        }
        result.files.push_back(pass.out);
        if (result.frames == 0) {
            result.frames = rendered.size() / 2;
            result.seconds = static_cast<double>(result.frames) / rate;
        }
    }
    std::filesystem::remove(staged);

    if (options.stem_mask != 0 && options.stem_zip) {
        std::filesystem::path zip_path = path;
        zip_path.replace_extension();
        zip_path += ".stems.zip";
        if (!zip_stems(zip_path, result.files, result.error)) {
            return result;
        }
        result.files.clear();
        result.files.push_back(zip_path);
    }

    result.ok = true;
    return result;
}

ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, ExportFormat format,
                            std::uint32_t rate, double tail_seconds) {
    ExportOptions options;
    options.format = format;
    options.sample_rate = rate;
    options.tail_seconds = tail_seconds;
    return export_project(project, extras, path, options);
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace nt::io
