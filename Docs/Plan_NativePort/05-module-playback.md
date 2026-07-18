# 05 — Module Playback and Import

## The problem being solved

The web app has no per-format replayer. Standard modules (MOD/XM/S3M/IT)
are *imported* — converted into NanoTracker's own effect model by four
hand-written parsers — and fidelity is capped by that mapping: S3M
Tremor dropped (`Source/federated-industries-main/src/lib/s3mImporter.ts:47`),
unknown XM effects silently zeroed (`xmImporter.ts:58`), panbrello
approximated (`itImporter.ts:82`), and the help window concedes the
importers are uneven (`TrackerHelp.tsx:1286`). This is the "reliable
tracker playback" pain point named in the port brief.

## Two distinct capabilities, two designs

### Playback: libopenmpt Module Player node

Faithful playback is delegated to libopenmpt (BSD-3, the reference
open-source replayer). It appears in the patch graph as a first-class
**Module Player** source node ([06-graph-cables.md](06-graph-cables.md)):

- Loads a module file, renders float stereo at the graph rate.
- Node controls from `openmpt::ext::interactive`: per-channel mute/solo,
  tempo/pitch factors, position/seek, current order/row readout for UI.
- Output is an ordinary audio port: route it through FX, sidechain it,
  record it — anything a cable can do.

This makes "play a module correctly" a solved problem rather than a
porting effort, and it gives NanoTracker a feature the web app never
had: dropping a module into a project as sound material without
conversion.

### Import: ported hand-written parsers

libopenmpt cannot be the import substrate: its public API deliberately
does not expose sample PCM, loop points, or instrument envelopes — the
project FAQ calls it a "pure module playback library", extraction
requests were declined upstream, and third parties fork it to get
extraction (verified 2026-07-17, Research/03). Import needs those, so
the four existing importers are ported — they already encode
NanoTracker's mapping decisions:

- `modImporter.ts` (4/6/8/xCHN, 15/31-sample), `xmImporter.ts`,
  `s3mImporter.ts`, `itImporter.ts` (incl. IT 2.14/2.15 sample
  decompression), shared helpers `importerUtils.ts`.

Fixes applied during the port (fix-list #1-3, #8):

- XM: validate the 0x0104 version field; header size read from the file
  (not hardcoded 40 — `xmImporter.ts:264`); relNote sign covered by an
  explicit regression test (the web fixed a sign bug at `:330`; the test
  pins it).
- Effects that were dropped/approximated get real implementations in the
  native engine where the engine's model can express them (Tremor,
  Panbrello, XM Tremolo as true tremolo); anything still inexpressible
  is reported in the import results dialog, never silently zeroed.
- S3M ADPCM samples decoded; pingpong loops imported as pingpong (the
  native sampler supports the mode natively rather than "forward only").
- Import warnings surface in one structured report (port of
  `importerUtils.ts:16-34` `ImportResults`), shown in the import modal.

### Import ≠ lossless, and that is fine

Import remains a *conversion* into NanoTracker's editing model; the
Module Player node is the fidelity path. The import dialog says exactly
that, so the web app's silent-degradation trap disappears as a matter of
UX as well as engineering.

## libopenmpt as a test oracle

Because libopenmpt links into the app anyway, tests use it as an
independent interpretation of the same file: render a module via the
Module Player node and via `openmpt123 --render`, null-test the outputs;
spot-check imported pattern data against
`openmpt::module::get_pattern_row_channel_command` where formats map
1:1. Mapping bugs in the ported importers show up as diffs instead of
sounding "a bit off" months later.

## Build note

libopenmpt has no upstream CMake: consumed via pkg-config
(`cmake/Findlibopenmpt.cmake`) with a vendored source fallback. Not
currently installed on the dev machine — Stage 0 handles acquisition.

## File map

- `src/modplay/module_player_node.{h,cpp}` — libopenmpt graph node
- `src/io/import/mod_importer.{h,cpp}`, `xm_importer.{h,cpp}`,
  `s3m_importer.{h,cpp}`, `it_importer.{h,cpp}`, `import_common.{h,cpp}`
- `src/ui/import_dialogs.cpp` — unified import-results report
- `tests/import_oracle.cpp` — libopenmpt cross-checks
