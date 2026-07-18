// Third-party single-file image codec implementation; compiled with
// warnings suppressed (see CMakeLists).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

#include "plugins/image_decode.h"

#include <cstring>

namespace nt::plugins {

bool decode_image_rgba(const std::uint8_t* data, std::size_t size, int& width, int& height,
                       std::vector<std::uint8_t>& rgba, std::string& error) {
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height,
                                            &channels, 4);
    if (pixels == nullptr) {
        error = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "undecodable image";
        return false;
    }
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    std::memcpy(rgba.data(), pixels, rgba.size());
    stbi_image_free(pixels);
    return true;
}

} // namespace nt::plugins
