#include "audio/decoders.h"

// Single-file codec implementations are instantiated here and only
// here; this TU builds with warnings disabled (see CMakeLists).
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>
#include <stb_vorbis.c>

namespace nt::audio::codec {

bool decode_wav(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error) {
    drwav wav;
    if (drwav_init_memory(&wav, data, size, nullptr) == DRWAV_FALSE) {
        error = "not a decodable WAV";
        return false;
    }
    out.frames = static_cast<std::uint32_t>(wav.totalPCMFrameCount);
    out.channels = wav.channels;
    out.rate = wav.sampleRate;
    out.interleaved.resize(static_cast<std::size_t>(out.frames) * out.channels);
    const drwav_uint64 read = drwav_read_pcm_frames_f32(&wav, out.frames, out.interleaved.data());
    drwav_uninit(&wav);
    if (read != out.frames) {
        error = "short WAV read";
        return false;
    }
    return out.frames > 0 && out.channels > 0;
}

bool decode_mp3(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error) {
    drmp3 mp3;
    if (drmp3_init_memory(&mp3, data, size, nullptr) == DRMP3_FALSE) {
        error = "not a decodable MP3";
        return false;
    }
    const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    out.frames = static_cast<std::uint32_t>(frames);
    out.channels = mp3.channels;
    out.rate = mp3.sampleRate;
    out.interleaved.resize(static_cast<std::size_t>(out.frames) * out.channels);
    const drmp3_uint64 read = drmp3_read_pcm_frames_f32(&mp3, frames, out.interleaved.data());
    drmp3_uninit(&mp3);
    if (read != frames) {
        error = "short MP3 read";
        return false;
    }
    return out.frames > 0 && out.channels > 0;
}

bool decode_ogg(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error) {
    int channels = 0;
    int rate = 0;
    short* pcm = nullptr;
    const int frames =
        stb_vorbis_decode_memory(data, static_cast<int>(size), &channels, &rate, &pcm);
    if (frames <= 0 || pcm == nullptr) {
        error = "not a decodable OGG";
        return false;
    }
    out.frames = static_cast<std::uint32_t>(frames);
    out.channels = static_cast<std::uint32_t>(channels);
    out.rate = static_cast<std::uint32_t>(rate);
    out.interleaved.resize(static_cast<std::size_t>(out.frames) * out.channels);
    for (std::size_t i = 0; i < out.interleaved.size(); ++i) {
        out.interleaved[i] = static_cast<float>(pcm[i]) / 32768.0F;
    }
    free(pcm);
    return true;
}

bool decode_ogg_comments(const std::uint8_t* data, std::size_t size, std::vector<std::string>& out,
                         std::string& error) {
    int open_error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, static_cast<int>(size), &open_error, nullptr);
    if (vorbis == nullptr) {
        error = "not a decodable OGG";
        return false;
    }
    const stb_vorbis_comment comment = stb_vorbis_get_comment(vorbis);
    out.clear();
    for (int i = 0; i < comment.comment_list_length; ++i) {
        if (comment.comment_list[i] != nullptr) {
            out.emplace_back(comment.comment_list[i]);
        }
    }
    stb_vorbis_close(vorbis);
    return true;
}

} // namespace nt::audio::codec
