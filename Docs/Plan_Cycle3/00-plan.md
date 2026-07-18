# Cycle 3 — Debt Paydown + Parked Features

Owner decisions (2026-07-18): all four parked features selected; debt
first, then features. Numbering continues; stage ritual unchanged
(verify → ledgers → git tag + tar). Cycle ships as 1.2.0; a 1.1.x
patch release closes the debt stage if it lands user-visible fixes.

## Stage 25 — Debt paydown

Every recorded small debt, one stage:
- **Record quantise precision**: EngineSnapshot publishes the
  sub-tick frame remainder (or last-tick stream frame);
  `app/midi_record` uses it — kills the up-to-one-tick early-read at
  row boundaries (punch-list C2; golden-traced RT path, so the
  change is snapshot-additive only).
- **Session preview-file API** (punch-list C3): reclaimer-backed
  decode-and-audition on ProjectSession; the sample browser stops
  caching decodes for process lifetime; the shell's play path joins.
- **Envelope stage persistence** (Stage 20 gap): per-instance env
  stage overrides in a new additive FTRK block (XENV or folded into
  a per-instance-state family), fixing both save→load reversion and
  the plugin-wide edit scope; loader applies overrides at
  instantiation.
- **native_stage state persistence** (Stage 21 gap): the same
  additive per-instance-state block family carries stage chunks
  (state_size/save/load already frozen in the ABI and tested).
- **MIDI last-note readout** updates from MIDI step-entry too
  (Stage 23 minor gap).
- FTRK format doc updated; reader stays back-compatible (files
  without the new block load unchanged).

## Stage 26 — Undo history panel

A HISTORY window over the existing UndoStack: entry list (labels
exist), click-to-jump (repeated undo/redo), depth indicator, sample-
op entries marked (bounded depth 8). Honest surface: structural edits
(sequence, patterns, workspace) currently CLEAR the stack — the
panel shows that truthfully ("history cleared: structural edit") and
records extending retention as follow-up, not silently promised.

## Stage 27 — Real transient slicing

- A spectral-flux onset detector (frame energy flux + adaptive
  threshold + minimum-gap) behind `sliceMap.autoDetect =
  "transients"` — replacing the grid:16 normalisation; deterministic,
  unit-tested against synthetic fixtures (clicks at known offsets ±
  tolerance, level-invariance).
- IT/WAV cue-marker slices: `"markers"` reads WAV cue chunks from
  the source sample (currently refused with a collected error).
- Slice preview in the sample view where the sampler node's source
  is visible (slice boundary overlay) if it fits the stage; else
  recorded.

## Stage 28 — Project browser + asset manager

- PROJECTS window: recent projects (persisted MRU with metadata:
  name, when, channels/patterns count), open/pin, new-from-template
  (default 4ch / 8ch).
- LIBRARY window: user library roots (settings-persisted dirs)
  listing samples (audition via the Stage 25 preview API), NTP
  archives (install = catalogue load), presets; favourites. Scope
  guard: a browser over real directories — no database, no tags
  beyond favourites.

## Stage 29 — Out-of-process plugin bridge

The heaviest parked item; gets its own design doc + research pass
before code (existing art: yabridge, Carla, plugin-sandbox models).
Target: external CLAP/VST3 instances host in a child process —
shared-memory audio/event rings sized to the block, editor embedding
cross-process (X11 reparent / Win32 SetParent both work across
processes), crash detection → auto-bypass with a loud node badge and
one-click restart. In-process remains the default; bridging is
per-plugin opt-in (settings), so the risk surface stays chosen.
Exit criteria include: a deliberately-crashing fixture plugin whose
death leaves audio running and the session saveable.

## Ordering rationale

25 first (debts unblock 27/28: preview API feeds the library's
audition; persistence blocks nothing but is the oldest promise).
26 next (small, self-contained). 27 before 28 only because its
detector work is isolated DSP; 28's library wants 25's preview API.
29 last: largest, riskiest, and its design doc should absorb
whatever the earlier stages reveal about instance lifecycle.
