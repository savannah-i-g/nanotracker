# 04 — Partitioned Convolution + FFT Library

Grounds Stage 20 (`../09-plugin-platform.md`): one convolution engine
serving the NTP convolver uncap and FX-mixer REVERB parity.

## FFT library candidates

| Library | Licence | SIMD | Notes |
| --- | --- | --- | --- |
| pffft (marton78 fork) | BSD-3-like (FFTPACK heritage) | SSE/NEON | fast, small, C with `pffft.hpp` C++ wrapper; real-input transforms; the maintained fork of Pommier's original |
| kissfft | BSD-3-Clause | none | simplest possible; measurably slower than pffft |
| FFTW | GPL | yes | licence-compatible with the GPLv3 app but heavy; overkill for fixed 128-frame blocks |

**Decision: pffft (marton78 fork), pinned.** Real-input FFTs at the
partition sizes we need, permissive licence for `LICENSES/`, no build
friction (single C file + header), SIMD without hand-rolling.

## Convolution scheme

- Literature baseline: partitioned convolution for real-time FIR is
  the Torger/Farina (2001) design — uniform block-size partitions,
  frequency-domain multiply-accumulate, overlap-add. Zero added
  latency when the partition equals the engine block (128 frames).
- **FFTConvolver** (HiFi-LoFi, MIT) is a proven C++ implementation of
  exactly this two-stage uniform scheme, used in shipping audio
  software — the design reference for our engine (we write our own to
  live inside the runner's RT discipline, but the partition/accum
  structure is validated there).
- Non-uniform partitioning (bigger FFTs for the IR tail) only if
  profiling demands it — matches the stage doc's "uniform first"
  call. pffft's companion PFFASTCONV confirms the library is used for
  FIR work at this scale.

## Plan implications

- `../09-plugin-platform.md` names pffft; `dsp_fft.{h,cpp}` wraps it
  behind our own interface so the library stays swappable.
- Null-test verification (engine vs direct FIR at short IRs) is
  well-posed: both paths compute the same convolution, so equality
  within float tolerance is the correct oracle.

## Sources

- https://github.com/marton78/pffft
- https://android.googlesource.com/platform/external/pffft/+/a748cde6e596bef575ba559b1097afab7e69711f/README.md
- https://github.com/HiFi-LoFi/FFTConvolver
- https://github.com/HiFi-LoFi/AudioFFT
- https://arxiv.org/pdf/2503.18022
