# 13 — Verification

## Principles

Every subsystem gets a verification anchor that does not depend on
"sounds right". The web app is a running reference implementation — use
it as an oracle wherever it is pure; use independent implementations
(libopenmpt) where it is not.

## Engine: golden vectors

The strongest anchor in the port
([04-engine.md](04-engine.md#golden-vector-testing)): the web
`advanceTick` is pure, so `tools/trace-dump/` runs it under Node to emit
per-tick JSON traces for scripted fixture projects; Catch2 replays them
against the C++ engine and requires exact equality. Fixture coverage:
every effect command, pattern-flow tortures (nested loops, break+jump
same row, pattern-delay stacking), bound-instrument resolution,
sequence-layer triggers, both freqTable modes.

## Formats

- FTRK: real v13 fixture projects (exported from the running web app)
  load → save → reload → deep-compare. Block-level fuzz: truncated and
  bit-flipped files must fail loudly (required-features bitmask,
  [10-formats-io.md](10-formats-io.md#ftrk-keep-and-extend)), never
  half-load.
- Importers: fixture modules per format; imported projects
  cross-checked against libopenmpt's pattern reads where 1:1, and
  rendered output diffed against `openmpt123 --render` as an oracle
  ([05-module-playback.md](05-module-playback.md#libopenmpt-as-a-test-oracle)).
- NTP: manifest validation suite — every rejection path has a test and
  a distinct, actionable message.

## Audio

- Module Player node null-tests against `openmpt123 --render`.
- Offline export of a fixture project is bit-identical across runs, and
  block-size-independent (rendering at 64 vs 128-frame blocks produces
  identical output) — proves the sample-clocked design.
- Xrun counter surfaced in the debug overlay; latency measured and
  displayed. A soak fixture (heavy project, feedback cables, module
  player + plugins) runs without xruns on the dev machine as a stage
  gate from Stage 7 onward.
- RT-safety: debug allocation assert on the audio thread; ASan/UBSan on
  the full test suite per stage close.

## UI

- Side-by-side screenshot parity against the running web app per view
  (pattern grid metrics, theme colors, channel palette).
- Keyboard model: scripted input-event tests against the pattern
  editor's edit state (cursor movement, hex entry, edit step).
- Cable interaction: registry-level tests (connect/reject/feedback
  placement) — the verlet visuals are judged by eye, the graph
  consequences by test.

### The input-script harness (the only sanctioned UI automation)

`nanotracker --input-script <file>` drives the app through ImGui's own
input queue (`app/input_script`): frame-paced key presses with
modifiers, self-written framebuffer screenshots (PPM), scripted quit
through the normal shutdown path. Grammar in `input_script.h`.

Rule, learned the hard way during Stage 4: never automate the user's
live desktop (xdotool/wmctrl window activation, cursor movement, XTEST
typing). Focus can land anywhere — including the user's terminal — and
synthetic events interact badly with the window manager. All UI
verification goes through the input script; it also works under
Wayland and Windows where desktop automation would not.

## Tooling

- Catch2 v3; `tests/` mirrors `src/` structure.
- Sanitizer CMake presets (`asan-ubsan`) from Stage 0.
- clang-tidy gate per stage close ([12-doctrine.md](12-doctrine.md)).
- `/verify`-style end-to-end pass at each stage: launch the app,
  exercise the stage's flow, observe behaviour — not just tests.
