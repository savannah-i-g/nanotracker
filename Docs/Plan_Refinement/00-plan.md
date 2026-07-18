# Refinement Cycle — Plan

Owner feedback (2026-07-18, post-1.0.0): refine to the standard of a
genuinely usable DAW. Numbering continues from the post-v1 cycle;
stage-close ritual unchanged (verify → ledgers → git tag + tar).

## Stage 22 — Appearance: Arctic Light default, UI scale, shell cleanup

- **Arctic Light theme** (owner's original web theme, JSON in this
  entry's commit): light background `#ecf4f8`, blue accents
  `#3388aa`, added to the palette table and made the DEFAULT for
  fresh configs (existing configs keep their saved choice). Full
  color set: primary #3388aa, primaryDim #99ccdd, primaryGlow
  rgba(51,136,170,0.08), bg #ecf4f8, bgElevated #e0ecf2, text
  #2277aa, textDim #88bbcc, border #b0d4e4, highlightBg #3388aa,
  highlightText #ecf4f8. The CRT pass must read sanely on a light
  theme (scanline rgba(51,136,170,0.02)) or default off for it.
- **UI scale**: a settings-persisted scale factor (SETTINGS menu +
  window), implemented by reloading the font atlas at
  base-size × scale plus ImGui style scaling — not
  FontGlobalScale blur. Default DOWN from today's oversized text;
  the pattern grid metrics and every view derived from font size
  must follow. Density: revisit default paddings/spacings at the
  same time (owner: "cramped").
- **Transport strip cleanup**: test tone and diagnostics move out of
  the shell into a DEBUG window (VIEW-gated, off by default);
  the shell keeps transport, BPM/SPD, position, status, meters.

## Stage 23 — Note entry + piano roll UX + module entry points

- **Module import vs player disambiguation** (owner 2026-07-18: "Mod
  import only gives us a player?"): the import pipeline is complete
  (io/import/ converts samples/patterns/order for MOD/XM/S3M/IT via
  FILE → open) but invisible next to the MODULE player window.
  Fix: FILE menu gains an explicit "import module…" item; opening a
  module file asks once — "Import as project" (default) / "Open in
  module player"; the MODULE window titles itself as a PLAYER and
  carries a one-line hint pointing at import; import surfaces its
  warnings (approximation counts) in a visible report, not just the
  load-warning list.

- **NOTE ENTRY window**: the missing feedback surface when entering
  notes in the pattern grid — current octave, step, velocity/volume
  default, last-entered note, an on-screen keyboard (click to enter
  exactly as typing would), entry-mode indicator (kbd/MIDI
  step/record). Docked into the bottom strip by default.
- **Piano roll consistency**: one obvious toolbar — add/select mode
  toggle, layer selector with add-affordance made visible, audition
  toggle (notes play on click/drag), play-from-here; drag-to-add with
  duration (click was add-only); consistent selection styling with
  the pattern grid; the channel/layer identity always visible in the
  window title bar region.
- Route both through existing session paths; input-script +
  screenshot verification per the house standard.

## Stage 24 — DAW polish sweep

- A UX audit pass over every window against the "genuinely usable
  DAW" bar: consistent labels/casing, tooltips on every non-obvious
  control, keyboard focus behaviour, drag/drop where natural
  (sample browser → slots exists as double-click; add drag), status
  feedback for every long action, empty-state hints ("no samples —
  open the browser"), window-title consistency. Audit produces a
  punch list in this directory; the punch list is the stage.
- Candidates already recorded: session-level preview-file API for
  browser audition; EngineSnapshot sub-tick remainder for record
  precision; envelope stage persistence (per-instance overrides);
  MIDI device drain hoisted out of the midi view.

Parked decisions for the owner to redirect: whether Arctic Light
default lands as 1.1.0 (behaviour-visible change) — plan assumes yes
with the theme picker keeping every existing theme one click away.
