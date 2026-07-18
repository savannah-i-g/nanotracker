// audio/convolution_engine — uniform partitioned FIR convolution
// (Torger/Farina scheme; structure validated against HiFi-LoFi's
// FFTConvolver, Docs/Plan_PostV1/Research/04). Serves the NTP
// convolver above its direct-FIR crossover and the FX-mixer REVERB's
// convolution mode.
//
// Partition math: the impulse is cut into P = ceil(taps / B)
// partitions of B = block_frames taps. Each partition is zero-padded
// to N = 2B and forward-transformed once at prepare() (pre-scaled by
// 1/N — pffft transforms are unnormalised). At run time the spectra of
// the last P input blocks sit in a ring, and each rendered block is
//
//     IFFT( sum over k of IR[k] * ring[(head + k) mod P] )
//
// overlap-added with the tail of the previous IFFT. `head` DECREMENTS
// as blocks complete, so the block k steps in the past is always at
// (head + k) mod P without any per-block re-copying of spectra.
//
// Zero added latency (FFTConvolver's two-stage trick): the tail
// partitions (k >= 1) involve only completed past blocks, so their sum
// is accumulated once per block — when its first samples arrive. The
// head partition (k = 0) involves the in-progress block, so on every
// process() call the partially filled segment is re-transformed and
// partition 0 re-applied. Output frame n therefore depends only on
// input frames <= n: correct for any call size, and exactly one
// FFT+IFFT per call when callers feed whole B-frame blocks.
//
// Threading: prepare() allocates and transforms — session/loader
// threads only. process()/reset() are RT-safe: no allocation, no
// locks, work bounded by P. Deterministic: pure float arithmetic over
// prepared spectra — one impulse + one input stream gives bit-identical
// output on a given build.
//
// Channels: the engine is mono. Stereo call sites run one engine per
// channel — both call sites keep per-channel impulses anyway, and a
// planar pair keeps this class trivial (no interleave variants).
#pragma once

#include "audio/dsp_fft.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace nt::audio {

class ConvolutionEngine {
public:
    // OFF-thread. Partitions and transforms `impulse` and allocates
    // every run-time buffer. `block_frames` must be a power of two
    // >= 16 (so N = 2B meets pffft's real-transform minimum of 32).
    // Returns false — engine not ready() — on an empty impulse or an
    // invalid block size. Re-preparing an engine is legal off-thread.
    bool prepare(std::span<const float> impulse, std::uint32_t block_frames);

    // RT-safe. Convolves `frames` input samples; any count works (the
    // engine segments internally — see the two-stage note above). `in`
    // and `out` may alias. Passes input through untouched when the
    // engine is not ready (callers normally guard on ready()).
    void process(const float* in, float* out, std::uint32_t frames);

    // RT-safe. Forgets all input history; the prepared impulse stays.
    void reset();

    [[nodiscard]] bool ready() const { return partition_count_ > 0; }

    [[nodiscard]] std::uint32_t impulse_frames() const { return impulse_frames_; }

private:
    std::uint32_t block_frames_ = 0;    // B: partition size == caller block
    std::uint32_t fft_size_ = 0;        // N = 2B
    std::uint32_t partition_count_ = 0; // P = ceil(taps / B); 0 = not ready
    std::uint32_t impulse_frames_ = 0;

    std::unique_ptr<dsp::RealFft> fft_;      // plan of size N (RealFft is pinned)
    std::vector<dsp::FftBuffer> ir_spectra_; // P impulse partitions, 1/N-scaled
    std::vector<dsp::FftBuffer> in_spectra_; // ring of past input spectra

    dsp::FftBuffer input_block_;    // N floats: [0,B) filling, [B,N) forever 0
    dsp::FftBuffer time_scratch_;   // N floats: IFFT landing zone
    dsp::FftBuffer tail_accum_;     // sum of partitions 1..P-1, once per block
    dsp::FftBuffer spectrum_accum_; // tail_accum_ + head partition, per call
    dsp::FftBuffer overlap_;        // B floats: previous IFFT's second half

    std::uint32_t fill_ = 0;      // filled frames of the current block
    std::uint32_t ring_head_ = 0; // in_spectra_ slot of the current block
};

} // namespace nt::audio
