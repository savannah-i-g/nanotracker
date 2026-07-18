# 05 — LUFS / BS.1770 Loudness Measurement

Grounds Stage 15 (`../04-export-suite.md`): the export suite's
peak / true-peak / LUFS normalisation options.

## Headline facts

| Question | Answer |
| --- | --- |
| Library | libebur128 (jiixyj) — the de-facto open implementation of EBU R 128 / ITU-R BS.1770 |
| Licence | MIT |
| Coverage | momentary (M), short-term (S), integrated (I) loudness; loudness range (EBU TECH 3342); **true-peak scanning included**; all sample rates via filter-coefficient recalculation |
| Conformance | passes EBU conformance checking (alongside FFmpeg's implementation) |
| Integration | plain C API, documented in `ebur128.h`; minimal-example in tree; trivial FetchContent target |

## Decision

**Use libebur128; do not self-implement BS.1770.** The K-weighting
filter, gating blocks, and true-peak oversampler are all conformance
trap-doors; a conformance-passing MIT library removes the entire risk
class. Our export post-processing feeds it the offline-rendered
float32 stream and applies gain from the integrated-loudness result.

Note: true peak comes from the same library call family, so the
export suite's peak/true-peak/LUFS trio needs exactly one dependency.

## Plan implications

- `../04-export-suite.md` names libebur128 (MIT → `LICENSES/`).
- Verification stays as planned: known-level fixtures (e.g. -23 LUFS
  sine/noise renders) asserted against the library's integrated
  reading, then against our normaliser's output level.

## Sources

- https://github.com/jiixyj/libebur128
- https://github.com/jiixyj/libebur128/blob/master/ebur128/ebur128.h
- https://tech.ebu.ch/docs/r/r128.pdf
- https://en.wikipedia.org/wiki/EBU_R_128
