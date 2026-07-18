// audio/decoders — thin wrappers over the vendored single-file codecs
// (dr_wav, dr_mp3, stb_vorbis). The implementations live in
// decoders.cpp, compiled with warnings suppressed so third-party code
// does not pollute the project's warning gate; nothing else in the
// tree includes those headers' implementation macros.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt::audio::codec {

struct Decoded {
    std::vector<float> interleaved; // frames * channels
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
};

bool decode_wav(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error);
bool decode_mp3(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error);
bool decode_ogg(const std::uint8_t* data, std::size_t size, Decoded& out, std::string& error);

} // namespace nt::audio::codec
