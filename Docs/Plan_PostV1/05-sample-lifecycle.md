# 05 — Sample Lifecycle + Waveform Editing (Stage 16)

Two halves with a hard dependency between them: destructive waveform
editing needs an undo model over sample buffers, and an undo model
needs real buffer lifetime semantics — which v1 deliberately did not
build ("samples are never freed until shutdown", the oldest stale
promise: `src/audio/audio_engine.h:74`, `src/app/main.cpp:154`,
`src/app/project_session.h:246`).

## First: deferred sample reclamation

Not a leak patch — a concurrency design task. The audio callback
holds raw `SampleBuffer*` in voices and bundles; freeing on the UI
thread invites use-after-free.

Design: generation-fenced retirement matching the existing bundle
discipline (`project_session.h` retired_ vectors are the pattern):
- Buffers retire into a pending list tagged with the engine's pull
  counter (already published in the snapshot as `pulls`).
- Reclamation frees a retired buffer only after observing a snapshot
  whose pull count proves the audio thread has swapped past every
  bundle that referenced it (RCU-style grace period).
- Voices never outlive their bundle's buffers by construction
  (kSetBundle already deactivates voices; kSwapBundle keeps the same
  sample set — enforced by the publish paths).
- The session's `buffers_` slots become replaceable without leaking;
  `retired_bundles_`/`retired_racks_`/`retired_runners_` join the
  same reclamation sweep instead of growing forever.

## Then: destructive waveform ops

Behavioural reference: `components/WaveformEditor.tsx` (1023 LOC) —
`toolTrim:233`, silence `:245`, fades `:251,:260`, normalize `:269`,
reverse `:284`, gain ±dB `:294`, DC removal `:314`, zoom/selection,
shortcuts `:842`.

- Ops mutate a *new* buffer + re-encode `original_data` (WAV PCM16
  write path exists in the export writer) so FTRK persistence stays
  true to what plays; the old buffer retires via the new lifecycle.
- Undo: each op pushes the previous buffer + metadata into the undo
  stack (bounded — buffer snapshots are the honest cost; cap depth
  for sample ops specifically).
- Selection + zoom in `src/ui/sample_view.*`: click-drag selection
  over the waveform, zoom-to-selection, op toolbar.

## Sample browser

A filesystem browser window (directory listing, audition on select
through the preview path) replacing load-by-path typing. Native
paths only — the web's File System Access model doesn't apply.

## File map

- `src/audio/sample_reclaim.{h,cpp}` — generation-fenced retirement
- `src/ui/sample_view.{h,cpp}` — selection/zoom/toolbar (extended)
- `src/app/project_session.{h,cpp}` — destructive-op entry points
- `src/ui/sample_browser_view.{h,cpp}` — browser window (new)

## Verification

Reclamation: a stress unit test cycling load/replace/play under the
live engine with ASan (the suite runs ASan already); assert retired
buffers actually free (counter) and never while referenced. Ops:
deterministic unit tests per op (trim lengths, normalize peak,
reverse symmetry, DC mean ≈ 0) + undo round-trips + FTRK re-encode
round-trip. UI via input-script.
