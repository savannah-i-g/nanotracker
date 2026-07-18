#include "audio/dsp_fft.h"

#include <algorithm>
#include <cassert>
#include <pffft/pffft.h>

namespace nt::audio::dsp {

// NOLINTBEGIN(cppcoreguidelines-owning-memory) — FftBuffer is the RAII
// owner over pffft's C allocator pair; there is no C++ owner type to
// hand the pointer to.

FftBuffer::FftBuffer(std::size_t floats) : size_(floats) {
    if (floats == 0) {
        return;
    }
    data_ = static_cast<float*>(pffft_aligned_malloc(floats * sizeof(float)));
    zero();
}

FftBuffer::~FftBuffer() {
    pffft_aligned_free(data_);
}

FftBuffer::FftBuffer(FftBuffer&& other) noexcept : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

FftBuffer& FftBuffer::operator=(FftBuffer&& other) noexcept {
    if (this != &other) {
        pffft_aligned_free(data_);
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void FftBuffer::zero() {
    std::fill_n(data_, size_, 0.0F);
}

// NOLINTEND(cppcoreguidelines-owning-memory)

bool RealFft::valid_size(std::uint32_t size) {
    return size >= kMinRealFftSize && (size & (size - 1)) == 0;
}

RealFft::RealFft(std::uint32_t size) : size_(size) {
    assert(valid_size(size));
    setup_ = pffft_new_setup(static_cast<int>(size), PFFFT_REAL);
    assert(setup_ != nullptr);
    // pffft only *needs* a caller work area for N >= 16384 (it would
    // otherwise alloca on the stack) — providing one unconditionally
    // keeps audio-thread stack use flat and size-independent.
    work_ = FftBuffer(size);
}

RealFft::~RealFft() {
    if (setup_ != nullptr) {
        pffft_destroy_setup(setup_);
    }
}

void RealFft::forward(const float* time, float* spectrum) {
    pffft_transform(setup_, time, spectrum, work_.data(), PFFFT_FORWARD);
}

void RealFft::inverse(const float* spectrum, float* time) {
    pffft_transform(setup_, spectrum, time, work_.data(), PFFFT_BACKWARD);
}

void RealFft::convolve_accumulate(const float* a, const float* b, float* accumulator, float scale) {
    pffft_zconvolve_accumulate(setup_, a, b, accumulator, scale);
}

} // namespace nt::audio::dsp
