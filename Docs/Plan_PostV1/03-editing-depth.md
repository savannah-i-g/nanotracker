# 03 — Editing Depth (Stage 14)

The single most disqualifying daily-driver gap: a tracker without
cell copy/paste cannot be anyone's default. Pure UI + clipboard +
transform work over pattern/sequence data — no lifecycle risk (that
is deliberately Stage 16's problem).

## Pattern grid (`src/ui/pattern_view.*`)

Behavioural reference: `components/TrackerCanvas.tsx` (single-cell
clipboard `:510-520`) — and where the web is thin, exceed it rather
than port the thinness (fix-don't-retain applies to missing depth
too; classic tracker selection semantics are the reference then):

- Selection blocks: shift+arrows / mouse drag over the grid;
  per-column-aware (a selection can span note/ins/vol/fx columns).
- Copy / cut / paste of cells and blocks through an internal
  clipboard (project-native structs, not text).
- Transpose selection ±1 semitone / ±1 octave.
- Interpolate: linear fill of volume or effect-param columns across
  a selection.
- All routed through `ProjectSession::set_cell` so every mutation is
  undoable cell-by-cell; block operations batch into one undo entry
  (extend `app/undo.h` with a grouping helper).

## Piano roll (`src/ui/piano_roll_view.*`)

Behavioural reference: `components/SequencePianoRollEditor.tsx`
(toolbox `:788-799`, copy/paste `:603-619`):

- Multi-select (click-drag rubber band + shift-click).
- Toolbox: quantize, humanize, transpose ±octave/+5th, reverse,
  invert, arpeggiate, velocity curve, gate length.
- Copy/paste of note sets.
- Note drag (move) and tail drag (resize) — currently add/remove only.
- Sequence edits stay structural (stop + republish, per the Stage 11
  threading note in PROGRESS); tool operations batch the stop.

## Fold-ins (backends already exist)

- INSTRUMENTS window source picker: sample | plugin | workspace type
  selection per slot (`src/ui/instrument_table_view.cpp` — the
  binding path through `project_session.cpp` bind_plugins is fully
  live; only the UI is missing). Pays a stale promise.
- Ballistic peak meter for the shell audio panel (`src/app/main.cpp`)
  — attack-fast/release-slow filter + draw; share with the FX mixer
  strips if trivial.

## Verification

Input-script runs: scripted selection + copy/paste + transpose with
screenshots and project-state assertions via a `--dump-cells` style
hook or unit-level session tests (block ops through
`ProjectSession` are unit-testable without UI). Piano-roll toolbox:
unit tests on the transform functions (quantize/humanize with seeded
determinism) + a screenshot run.
