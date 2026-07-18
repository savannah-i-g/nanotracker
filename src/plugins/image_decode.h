// plugins/image_decode — PNG/JPG decode for plugin UI assets, wrapping
// the vendored stb_image (implementation TU compiled with warnings
// suppressed, matching audio/decoders' treatment of third-party code).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nt::plugins {

// Decodes to tightly-packed RGBA8. Returns false with `error` set on
// unsupported or corrupt data.
bool decode_image_rgba(const std::uint8_t* data, std::size_t size, int& width, int& height,
                       std::vector<std::uint8_t>& rgba, std::string& error);

} // namespace nt::plugins
