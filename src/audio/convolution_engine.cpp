#include "audio/convolution_engine.h"

#include <algorithm>
#include <cstring>

namespace nt::audio {

bool ConvolutionEngine::prepare(std::span<const float> impulse, std::uint32_t block_frames) {
    // Invalidate first so a failed re-prepare leaves a non-ready
    // engine, never a half-swapped one.
    partition_count_ = 0;
    impulse_frames_ = 0;

    const std::uint32_t fft_size = block_frames * 2;
    if (impulse.empty() || !dsp::RealFft::valid_size(fft_size)) {
        return false;
    }
    block_frames_ = block_frames;
    fft_size_ = fft_size;
    impulse_frames_ = static_cast<std::uint32_t>(impulse.size());
    const std::uint32_t partitions = (impulse_frames_ + block_frames_ - 1) / block_frames_;

    fft_ = std::make_unique<dsp::RealFft>(fft_size_);
    input_block_ = dsp::FftBuffer(fft_size_);
    time_scratch_ = dsp::FftBuffer(fft_size_);
    tail_accum_ = dsp::FftBuffer(fft_size_);
    spectrum_accum_ = dsp::FftBuffer(fft_size_);
    overlap_ = dsp::FftBuffer(block_frames_);

    // Impulse partitions: B taps zero-padded to N, transformed once.
    // The 1/N inverse-transform normalisation is baked in here so the
    // run-time accumulate/inverse chain needs no extra scaling pass.
    ir_spectra_.clear();
    in_spectra_.clear();
    ir_spectra_.reserve(partitions);
    in_spectra_.reserve(partitions);
    dsp::FftBuffer segment(fft_size_);
    const float inv_n = 1.0F / static_cast<float>(fft_size_);
    for (std::uint32_t p = 0; p < partitions; ++p) {
        segment.zero();
        const std::uint32_t begin = p * block_frames_;
        const std::uint32_t count = std::min(block_frames_, impulse_frames_ - begin);
        for (std::uint32_t i = 0; i < count; ++i) {
            segment.data()[i] = impulse[begin + i] * inv_n;
        }
        dsp::FftBuffer spectrum(fft_size_);
        fft_->forward(segment.data(), spectrum.data());
        ir_spectra_.push_back(std::move(spectrum));
        in_spectra_.emplace_back(fft_size_); // zero spectrum == silence
    }

    fill_ = 0;
    ring_head_ = 0;
    partition_count_ = partitions;
    return true;
}

void ConvolutionEngine::reset() {
    if (!ready()) {
        return;
    }
    for (dsp::FftBuffer& spectrum : in_spectra_) {
        spectrum.zero();
    }
    input_block_.zero(); // [B,N) stays zero by invariant; cheap to re-clear all
    overlap_.zero();
    fill_ = 0;
    ring_head_ = 0;
}

void ConvolutionEngine::process(const float* in, float* out, std::uint32_t frames) {
    if (!ready()) {
        if (out != in) {
            std::memmove(out, in, static_cast<std::size_t>(frames) * sizeof(float));
        }
        return;
    }
    std::uint32_t processed = 0;
    while (processed < frames) {
        const bool block_was_empty = fill_ == 0;
        const std::uint32_t chunk = std::min(frames - processed, block_frames_ - fill_);

        // Stage the chunk; from here on we read only staged copies, so
        // in/out aliasing is safe.
        std::copy_n(in + processed, chunk, input_block_.data() + fill_);

        // Head spectrum: the (partial) current block, re-transformed
        // every call — the price of zero latency below full blocks.
        fft_->forward(input_block_.data(), in_spectra_[ring_head_].data());

        // Tail spectra involve only completed blocks: accumulate once
        // per block, at its first chunk. ring[(head + k) mod P] is the
        // block k steps in the past (head decrements at block close).
        if (block_was_empty) {
            tail_accum_.zero();
            for (std::uint32_t k = 1; k < partition_count_; ++k) {
                const std::uint32_t slot = (ring_head_ + k) % partition_count_;
                fft_->convolve_accumulate(in_spectra_[slot].data(), ir_spectra_[k].data(),
                                          tail_accum_.data(), 1.0F);
            }
        }

        std::copy_n(tail_accum_.data(), fft_size_, spectrum_accum_.data());
        fft_->convolve_accumulate(in_spectra_[ring_head_].data(), ir_spectra_[0].data(),
                                  spectrum_accum_.data(), 1.0F);
        fft_->inverse(spectrum_accum_.data(), time_scratch_.data());

        // Overlap-add against the previous block's tail. Only the
        // window this chunk uncovered is emitted; earlier frames of
        // the block went out on earlier calls.
        for (std::uint32_t i = 0; i < chunk; ++i) {
            out[processed + i] = time_scratch_.data()[fill_ + i] + overlap_.data()[fill_ + i];
        }

        fill_ += chunk;
        processed += chunk;
        if (fill_ == block_frames_) {
            // Block complete: keep its IFFT tail for the next block's
            // overlap-add, clear the staging area, retire the slot.
            std::copy_n(time_scratch_.data() + block_frames_, block_frames_, overlap_.data());
            std::fill_n(input_block_.data(), block_frames_, 0.0F);
            fill_ = 0;
            ring_head_ = ring_head_ == 0 ? partition_count_ - 1 : ring_head_ - 1;
        }
    }
}

} // namespace nt::audio
