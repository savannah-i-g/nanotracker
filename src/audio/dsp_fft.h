// audio/dsp_fft — real-valued FFT primitives for the convolution
// engine, wrapping pffft behind a project-owned interface so the
// library stays swappable (Docs/Plan_PostV1/Research/04). Nothing
// outside this pair of files includes a pffft header.
//
// Contract (inherited from pffft, enforced or narrowed here):
// - Transform sizes are powers of two >= kMinRealFftSize (32, pffft's
//   real-transform minimum). pffft also accepts 3^b/5^c factors; this
//   wrapper deliberately does not — the convolution engine only ever
//   asks for 2 * block_frames.
// - Every float* handed to forward/inverse/convolve_accumulate must be
//   SIMD-aligned (16 bytes on x86/ARM; FftBuffer allocates with pffft's
//   own 64-byte-aligned allocator). Stack arrays and std::vector
//   storage do NOT qualify.
// - Spectra use pffft's internal (unordered) layout: `size` floats with
//   DC and Nyquist packed into the first complex bin. The layout is
//   opaque here — spectra only ever feed convolve_accumulate() and
//   inverse(), which understand it. Elementwise copy/add/zero remain
//   legal (the layout is a permutation of the ordered bins).
// - Transforms are unnormalised: inverse(forward(x)) == size * x.
//   Callers fold the 1/size factor wherever it is cheapest (the
//   convolution engine bakes it into the stored impulse spectra).
#pragma once

#include <cstddef>
#include <cstdint>

// pffft's opaque setup type ("typedef struct PFFFT_Setup PFFFT_Setup"
// in C). Forward-declared at global scope so this header needs no
// pffft include.
struct PFFFT_Setup;

namespace nt::audio::dsp {

inline constexpr std::uint32_t kMinRealFftSize = 32;

// SIMD-aligned float storage (move-only RAII). Zero-filled at
// construction; zero() re-clears without reallocating.
class FftBuffer {
public:
    FftBuffer() = default;
    explicit FftBuffer(std::size_t floats);
    ~FftBuffer();

    FftBuffer(FftBuffer&& other) noexcept;
    FftBuffer& operator=(FftBuffer&& other) noexcept;
    FftBuffer(const FftBuffer&) = delete;
    FftBuffer& operator=(const FftBuffer&) = delete;

    [[nodiscard]] float* data() { return data_; }

    [[nodiscard]] const float* data() const { return data_; }

    [[nodiscard]] std::size_t size() const { return size_; }

    void zero();

private:
    float* data_ = nullptr;
    std::size_t size_ = 0;
};

// One fixed-size real transform plan (setup + work area). The plan is
// immutable after construction; forward/inverse mutate only the work
// scratch, never allocate, and are safe on the audio thread.
class RealFft {
public:
    // `size` must satisfy valid_size(); the plan asserts on violation.
    explicit RealFft(std::uint32_t size);
    ~RealFft();

    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;
    RealFft(RealFft&&) = delete;
    RealFft& operator=(RealFft&&) = delete;

    [[nodiscard]] static bool valid_size(std::uint32_t size);

    // time (size floats) → spectrum (size floats, unordered layout).
    void forward(const float* time, float* spectrum);

    // spectrum (unordered) → time, scaled by `size` (see header note).
    void inverse(const float* spectrum, float* time);

    // accumulator += (a * b) * scale, all three in the unordered
    // spectral layout of THIS plan. Pointers may alias.
    void convolve_accumulate(const float* a, const float* b, float* accumulator, float scale);

    [[nodiscard]] std::uint32_t size() const { return size_; }

private:
    ::PFFFT_Setup* setup_ = nullptr;
    FftBuffer work_; // per-transform scratch; heap, so no audio-thread alloca
    std::uint32_t size_ = 0;
};

} // namespace nt::audio::dsp
