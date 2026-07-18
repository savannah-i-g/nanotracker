// io/sha256 — SHA-256 (FIPS 180-4) over in-memory bytes.
// POVR sample overrides key on "sha256:<hex>" content hashes (the
// web hashed with WebCrypto; files must agree byte-for-byte across
// hosts, so this is the one hash the project defines). Plain C++,
// no dependencies; runs on loader/session threads only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nt::io {

[[nodiscard]] std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size);

// Lowercase-hex digest with the POVR "sha256:" prefix.
[[nodiscard]] std::string sha256_hex_prefixed(const std::uint8_t* data, std::size_t size);

} // namespace nt::io
