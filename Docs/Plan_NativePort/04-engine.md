# 04 — Tracker Engine

## What is being ported

The web engine is the cleanest part of the source app: a pure state
machine with an explicit contract ("no side effects, no audio, no DOM" —
`Source/federated-industries-main/src/lib/trackerEngine.ts:1-5`). The
port preserves that purity in `src/engine/`, which depends on nothing
but the standard library.

Data model (web citations; native equivalents keep the semantics):

- `TrackerCell` (`trackerEngine.ts:13-28`): note 0/1-96/97(off),
  instrument 0-31, volume 0xFF-default or 0x00-0x40, effect nibble +
  param byte, optional bound-instrument sub-slot.
- `TrackerPattern` (`:30-38`): rows[row][channel], per-pattern highlight
  overrides.
- `TrackerSample` (`:44-61`): original encoded bytes + rate/frames/loop/
  baseNote/finetune/volume/pan/stretch/category.
- `InstrumentTableEntry` (`:70-97`): unified sample|plugin|workspace
  slots, bound tracks.
- `TrackerProject` (`:99-135`): bpm, speed (ticks/row), order list,
  patterns, samples, freqTable amiga|linear, fxMixer, workspace,
  sequenceMixer, channelColors.
- Sequence layers (piano roll): `trackerSeqEngine.ts` — max 4 layers per
  channel, notes with tick positions; FX automation: `trackerFxEngine.ts`.

Effect interpreter: full ProTracker set 0x0-0xF including E-extended
(arpeggio, portamentos, vibrato/tremolo, volume slides, sample offset,
position jump, pattern break/loop/delay, retrig, note cut/delay, fine
variants; speed<0x20 / BPM>=0x20 split) — `trackerEngine.ts:562-770`.
Defaults BPM 125, speed 6, 64 rows, 4 channels (`:424-456`).

## Transport

Timing law: tick interval = 2500/bpm ms; row = speed ticks; 24 PPQN for
clock consumers. Natively the sequencer runs on the audio thread: the
block loop converts tick interval to frames at the device rate,
accumulates a frame counter, and calls the `advance_tick` state machine
exactly at tick boundaries, splitting DSP blocks across a boundary when
needed ([03-audio-backend.md](03-audio-backend.md#block-model)).

Not ported: the entire lookahead scheduler
(`src/lib/transportClock.ts:139-311` — 0.12s lookahead, worklet clock
pump, 500-tick catch-up cap, jitter diagnostics). All of it compensates
for the browser's inability to run the sequencer in the audio callback.
Native trackers (ft2-clone, libopenmpt itself) sequence in the callback;
so do we. The 24 PPQN position bookkeeping survives as plain counters
for the MIDI clock ([11-midi.md](11-midi.md)).

## Sampler runtime

Port of `samplerRuntime.ts` semantics with two deliberate fixes:

- Deterministic priming (fix-list #4): a note never silently plays a
  null buffer because decode wasn't ready
  (`samplerRuntime.ts:212-244`). Samples are decoded/resampled by the
  loader pool at load time; an instrument is not triggerable until its
  buffers are resident, and the UI shows loading state.
- Loop points in play-buffer frames (fix-list #9): the web stores loop
  points in source-rate frames and divides by the source rate at play
  time (`trackerAudio.ts:660-668`), which only works because importers
  maintain a compensating invariant (`modImporter.ts:242-243`). The
  native sampler resamples at load and stores loop points in frames of
  the resident buffer. The FTRK fields keep source-rate semantics for
  compatibility; conversion happens at load.

Pitch: period tables for amiga/linear freqTable modes ported verbatim
from the web engine; finetune in cents; `stretchRatio` honoured.

## Golden-vector testing

The single highest-value verification asset in the port: because both
engines are pure, the web `advanceTick` can be run under Node to dump
per-tick JSON traces (channel trigger flags, period/volume overrides,
row/order transitions) for scripted projects covering the whole effect
matrix. The C++ engine must reproduce the traces exactly.

- `tools/trace-dump/` — small Node script importing
  `src/lib/trackerEngine.ts` from the web repo, emitting
  `tests/golden/*.json`.
- `tests/engine_golden.cpp` — Catch2 driver replaying traces against
  `src/engine/`.
- Coverage: one fixture per effect command, plus pattern-flow tortures
  (nested loops, break+jump on the same row, pattern delay stacking) and
  the sequence-layer trigger paths (`trackerSeqEngine.ts`
  `findNotesStartingAtTick/EndingAtTick`).

## File map

- `src/engine/tracker_types.h` — cells, patterns, samples, project
- `src/engine/tracker_engine.{h,cpp}` — advance_tick state machine
- `src/engine/seq_engine.{h,cpp}` — piano-roll layers
- `src/engine/fx_engine.{h,cpp}` — FX-mixer automation
- `src/audio/sampler_voice.{h,cpp}` — voice playback (audio side)
- `tools/trace-dump/` + `tests/engine_golden.cpp`

Related: [03-audio-backend.md](03-audio-backend.md),
[13-verification.md](13-verification.md).
