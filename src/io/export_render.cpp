#include "io/export_render.h"

#include "app/project_session.h"
#include "audio/audio_engine.h"
#include "platform/shared_library.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vorbis/vorbisenc.h>

namespace nt::io {

// Codec/file interop: WAV headers, ogg_page buffers and stream writes
// all cross C APIs through byte pointers — reinterpret_cast is the
// honest spelling at those seams.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

namespace {

constexpr std::uint64_t kMaxFrames = 48000ULL * 600ULL; // 10 minute safety cap

// ── WAV (PCM16) ──────────────────────────────────────────────────────

bool write_wav(const std::filesystem::path& path, const std::vector<float>& interleaved,
               std::uint32_t rate, std::string& error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    const auto frames = static_cast<std::uint32_t>(interleaved.size() / 2);
    const std::uint32_t data_bytes = frames * 4;
    auto u32 = [&file](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&file](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(36 + data_bytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    file.write("data", 4);
    u32(data_bytes);
    for (const float sample : interleaved) {
        const auto v = static_cast<std::int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
        u16(static_cast<std::uint16_t>(v));
    }
    if (!file.good()) {
        error = "write failed";
        return false;
    }
    return true;
}

// ── OGG (libvorbis) ──────────────────────────────────────────────────

bool write_ogg(const std::filesystem::path& path, const std::vector<float>& interleaved,
               std::uint32_t rate, std::string& error) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 2, static_cast<long>(rate), 0.5F) != 0) {
        vorbis_info_clear(&info);
        error = "vorbis encoder init failed";
        return false;
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_comment_add_tag(&comment, "ENCODER", "nanoTracker");
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
    int (*set_quality)(void*, int) = nullptr;
    int (*init_params)(void*) = nullptr;
    int (*encode_interleaved)(void*, short*, int, unsigned char*, int) = nullptr;
    int (*encode_flush)(void*, unsigned char*, int) = nullptr;
    int (*close)(void*) = nullptr;
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
    api.set_quality = reinterpret_cast<int (*)(void*, int)>(resolve("lame_set_quality"));
    api.init_params = reinterpret_cast<int (*)(void*)>(resolve("lame_init_params"));
    api.encode_interleaved = reinterpret_cast<int (*)(void*, short*, int, unsigned char*, int)>(
        resolve("lame_encode_buffer_interleaved"));
    api.encode_flush =
        reinterpret_cast<int (*)(void*, unsigned char*, int)>(resolve("lame_encode_flush"));
    api.close = reinterpret_cast<int (*)(void*)>(resolve("lame_close"));
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
               std::uint32_t rate, std::string& error) {
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
    api.set_in_samplerate(lame, static_cast<int>(rate));
    api.set_num_channels(lame, 2);
    api.set_quality(lame, 2);
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

} // namespace

ExportResult export_project(const engine::TrackerProject& project, const FtrkWriteExtras& extras,
                            const std::filesystem::path& path, ExportFormat format,
                            std::uint32_t rate, double tail_seconds) {
    ExportResult result;

    // Serialise → temp file → fresh offline session: the export renders
    // exactly what a load of the saved project would play.
    const std::vector<std::uint8_t> bytes = write_ftrk(project, extras);
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() / "nt_export_render.ftrk";
    {
        std::ofstream temp_file(temp, std::ios::binary);
        temp_file.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        if (!temp_file.good()) {
            result.error = "cannot stage temp project";
            return result;
        }
    }

    audio::AudioEngine engine;
    if (!engine.start_offline(rate)) {
        result.error = "offline engine start failed";
        return result;
    }
    app::ProjectSession session(engine);
    if (!session.load_ftrk(temp)) {
        result.error = "render load failed: " + session.error();
        return result;
    }
    session.play();

    std::vector<float> rendered;
    std::array<float, static_cast<std::size_t>(4096) * 2> chunk{};
    bool ended = false;
    std::uint64_t tail_frames = 0;
    const auto tail_total = static_cast<std::uint64_t>(tail_seconds * rate);
    while (rendered.size() / 2 < kMaxFrames) {
        engine.render_offline(chunk.data(), 4096);
        rendered.insert(rendered.end(), chunk.begin(), chunk.end());
        if (!ended && engine.snapshot().song_ended) {
            ended = true;
        }
        if (ended) {
            tail_frames += 4096;
            if (tail_frames >= tail_total) {
                break;
            }
        }
    }
    session.stop();

    bool ok = false;
    std::string encode_error;
    switch (format) {
    case ExportFormat::kWav:
        ok = write_wav(path, rendered, rate, encode_error);
        break;
    case ExportFormat::kOgg:
        ok = write_ogg(path, rendered, rate, encode_error);
        break;
    case ExportFormat::kMp3:
        ok = write_mp3(path, rendered, rate, encode_error);
        break;
    }
    result.ok = ok;
    result.error = encode_error;
    result.frames = rendered.size() / 2;
    result.seconds = static_cast<double>(result.frames) / rate;
    return result;
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace nt::io
