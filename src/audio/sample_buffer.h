// audio/sample_buffer — decoded, device-rate audio data.
// A SampleBuffer holds interleaved stereo float frames already
// resampled to the graph rate, so the render path never resamples.
// Loop points (when they arrive with the sampler stage) are stored in
// frames of THIS buffer — the web app's source-rate loop invariant is
// deliberately not carried over (fix-don't-retain #9).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nt::audio {

struct SampleBuffer {
    std::vector<float> interleaved; // stereo: frames * 2
    std::uint32_t frames = 0;
    std::uint32_t rate = 0; // always the rate it was resampled to
    std::uint32_t source_rate = 0;
    std::string name;
};

// Decodes a WAV file (dr_wav) and resamples to `target_rate`
// (libsamplerate, sinc medium). Mono sources are duplicated to stereo.
// Returns nullptr with a message in `error` on failure. Runs on a
// loader thread — never the audio thread.
std::unique_ptr<SampleBuffer> load_wav(const std::filesystem::path& path, std::uint32_t target_rate,
                                       std::string& error);

// Same pipeline for in-memory WAV bytes (project-embedded samples).
std::unique_ptr<SampleBuffer> load_wav_memory(const std::uint8_t* data, std::size_t size,
                                              std::uint32_t target_rate, std::string& error);

// Format-sniffing decode: WAV ("RIFF"), OGG Vorbis ("OggS"), else MP3.
// Detected format name lands in `format_out` ("wav"|"ogg"|"mp3").
std::unique_ptr<SampleBuffer> load_sample_memory(const std::uint8_t* data, std::size_t size,
                                                 std::uint32_t target_rate, std::string& format_out,
                                                 std::string& error);

} // namespace nt::audio
