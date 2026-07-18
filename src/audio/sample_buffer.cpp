#include "audio/sample_buffer.h"

#include "audio/decoders.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <samplerate.h>

namespace nt::audio {

namespace {

// Interleaved-stereo resample via libsamplerate's whole-buffer API.
std::vector<float> resample_stereo(const std::vector<float>& in, std::uint32_t in_frames,
                                   double ratio, std::uint32_t& out_frames, std::string& error) {
    const auto capacity =
        static_cast<std::uint32_t>(std::ceil(static_cast<double>(in_frames) * ratio)) + 16;
    std::vector<float> out(static_cast<std::size_t>(capacity) * 2);

    SRC_DATA data{};
    data.data_in = in.data();
    data.input_frames = static_cast<long>(in_frames);
    data.data_out = out.data();
    data.output_frames = static_cast<long>(capacity);
    data.src_ratio = ratio;

    if (const int err = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 2); err != 0) {
        error = src_strerror(err);
        return {};
    }
    out_frames = static_cast<std::uint32_t>(data.output_frames_gen);
    out.resize(static_cast<std::size_t>(out_frames) * 2);
    return out;
}

// Normalise decoded audio to interleaved stereo at target_rate.
std::unique_ptr<SampleBuffer> finish(const codec::Decoded& decoded, std::uint32_t target_rate,
                                     std::string& error) {
    // Stereo normalise (first two channels of multi-channel sources).
    std::vector<float> stereo(static_cast<std::size_t>(decoded.frames) * 2);
    for (std::size_t i = 0; i < decoded.frames; ++i) {
        const std::size_t src = i * decoded.channels;
        stereo[i * 2] = decoded.interleaved[src];
        stereo[(i * 2) + 1] =
            decoded.channels > 1 ? decoded.interleaved[src + 1] : decoded.interleaved[src];
    }

    auto buffer = std::make_unique<SampleBuffer>();
    buffer->source_rate = decoded.rate;
    buffer->source_channels = decoded.channels;
    buffer->rate = target_rate;
    if (decoded.rate == target_rate) {
        buffer->interleaved = std::move(stereo);
        buffer->frames = decoded.frames;
    } else {
        const double ratio = static_cast<double>(target_rate) / decoded.rate;
        buffer->interleaved = resample_stereo(stereo, decoded.frames, ratio, buffer->frames, error);
        if (buffer->interleaved.empty()) {
            return nullptr;
        }
    }
    return buffer;
}

} // namespace

std::unique_ptr<SampleBuffer> load_wav(const std::filesystem::path& path, std::uint32_t target_rate,
                                       std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    auto buffer = load_wav_memory(bytes.data(), bytes.size(), target_rate, error);
    if (buffer != nullptr) {
        buffer->name = path.filename().string();
    }
    return buffer;
}

std::unique_ptr<SampleBuffer> load_wav_memory(const std::uint8_t* data, std::size_t size,
                                              std::uint32_t target_rate, std::string& error) {
    codec::Decoded decoded;
    if (!codec::decode_wav(data, size, decoded, error)) {
        return nullptr;
    }
    return finish(decoded, target_rate, error);
}

std::unique_ptr<SampleBuffer> load_sample_memory(const std::uint8_t* data, std::size_t size,
                                                 std::uint32_t target_rate, std::string& format_out,
                                                 std::string& error) {
    codec::Decoded decoded;
    if (size >= 4 && std::memcmp(data, "RIFF", 4) == 0) {
        format_out = "wav";
        if (!codec::decode_wav(data, size, decoded, error)) {
            return nullptr;
        }
    } else if (size >= 4 && std::memcmp(data, "OggS", 4) == 0) {
        format_out = "ogg";
        if (!codec::decode_ogg(data, size, decoded, error)) {
            return nullptr;
        }
    } else {
        format_out = "mp3";
        if (!codec::decode_mp3(data, size, decoded, error)) {
            error = "unrecognised sample format (wav/ogg/mp3 supported): " + error;
            return nullptr;
        }
    }
    return finish(decoded, target_rate, error);
}

} // namespace nt::audio
