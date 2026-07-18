#include "io/sha256.h"

#include <bit>
#include <cstring>
#include <string_view>

namespace nt::io {

namespace {

// FIPS 180-4 section 4.2.2 round constants (cube roots of primes).
constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void compress(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = (static_cast<std::uint32_t>(block[t * 4]) << 24U) |
               (static_cast<std::uint32_t>(block[(t * 4) + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[(t * 4) + 2]) << 8U) |
               static_cast<std::uint32_t>(block[(t * 4) + 3]);
    }
    for (std::size_t t = 16; t < 64; ++t) {
        const std::uint32_t s0 =
            std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3U);
        const std::uint32_t s1 =
            std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10U);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t t = 0; t < 64; ++t) {
        const std::uint32_t big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + big_s1 + ch + kRound[t] + w[t];
        const std::uint32_t big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t size) {
    // FIPS 180-4 section 5.3.3 initial hash (square roots of primes).
    std::array<std::uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) {
        compress(state, data + offset);
    }
    // Final block(s): 0x80 terminator, zero pad, 64-bit big-endian
    // message length in bits.
    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = size - offset;
    if (remainder > 0) {
        std::memcpy(tail.data(), data + offset, remainder);
    }
    tail[remainder] = 0x80;
    const std::size_t tail_len = remainder + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_len - 1 - static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
    compress(state, tail.data());
    if (tail_len == 128) {
        compress(state, tail.data() + 64);
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] >> 24U);
        digest[(i * 4) + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFF);
        digest[(i * 4) + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFF);
        digest[(i * 4) + 3] = static_cast<std::uint8_t>(state[i] & 0xFF);
    }
    return digest;
}

std::string sha256_hex_prefixed(const std::uint8_t* data, std::size_t size) {
    const std::array<std::uint8_t, 32> digest = sha256(data, size);
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "sha256:";
    out.reserve(7 + 64);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4U]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

} // namespace nt::io
