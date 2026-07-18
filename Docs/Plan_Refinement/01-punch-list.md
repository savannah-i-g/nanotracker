# Stage 24 — DAW Polish Sweep — Punch List

UX audit of every window against the "genuinely usable DAW" bar. Audited
with the input-script harness under Xvfb (fresh config, `--no-crt`),
Arctic Light and Amber, at UI scale 1.0 and 1.5. Ranked
**broken > misleading > missing affordance > cosmetic**. Each item is
executed top-down; deliberate leaves carry a one-line reason.

Screenshot evidence lives in the scratchpad `shots/` (before/after where
meaningful).

## Owner instruction (highest priority — DONE)

- [x] **O1 — pattern/order-list structure editing end to end** (owner:
  "no way to actually add/remove patterns from the list! limited to just
  the one"). Session ops `create_pattern` / `delete_pattern` (reindexes
  patterns + parallel seq/fx + order refs, refuses the last) /
  `resize_pattern` (1..256, pad/truncate + seq-note clamp) / `order_insert`
  / `order_remove` (refuses emptying) / `order_move` / `order_set` /
  `set_order_list`. PATTERN-window ORDER column grew + / − / ↑ / ↓, an
  add popup ("new pattern" / insert existing PATnn), and a `< PATnn >`
  modulo repoint cycle. Local API's six matching ops flipped from
  `unsupported` to real (typed validation, `createdPatternIds` in the
  batch result, schema updated). Web deletePattern's id-gap bug fixed by
  reindexing (FIXES.md). Row-count IS per-pattern in the native model, so
  resize is included, not faked. 5 new tests (4 session + 1 Local API
  loopback); both trees 138/138; screenshot-verified add flow.

## Broken

- [x] **B1 — MIDI device intake stalls when the midi window is hidden.**
  `MidiView::draw` calls `drain_input` as its first line, and `draw` only
  runs while `vis.midi` is true (`main.cpp`). Hide the window (VIEW menu)
  and device MIDI is never polled — notes pile in the ring, nothing
  previews/records. Documented in the `main.cpp` comment as a known
  wart. Fix: hoist the drain to an unconditional per-frame call,
  independent of window visibility, preserving device-before-bus order.

## Misleading (clipping / wrong info)

- [x] **B2 — shell version+fps line clips at high UI scale.** The
  right-aligned `nanoTracker 1.0.0  NN fps` on the transport row is
  forced onto the same line as the transport controls; at 1.5 it runs
  past the strip's right edge ("fps" cut) and spawns a spurious
  horizontal scrollbar on the transport strip. Fix: adaptive placement
  (right-align on the row only when it fits, else its own line) and a
  shorter string, so it can never force the window wider. (KNOWN item,
  owner flagged.)
- [x] **B3 — LOCAL API right-rail clips its labels.** Docked in the
  ~290 px right rail, the token field ("token" → "toke") and the
  "localhost WebSocket, bearer-token auth" caption overflow at scale
  1.0. The token is the one value a user must read/copy. Fix: caption on
  its own line, token field stretches to available width.
- [x] **B4 — right rail too narrow for its content at scale.** The 0.24
  right-rail split clips midi input-mode radios ("enter"/"record") and
  the "MIDI learn" separator at 1.5. Fix: widen the default split and
  keep rail content within the rail width.

## Missing affordance / visibility

- [x] **B5 — channel instrument-pill text is low-contrast on light.**
  The filled pill draws its hex in `theme.background` unconditionally;
  on Arctic Light that is near-white text over a bright hue (green /
  cyan / yellow pills unreadable). Fix: pick pill text by hue luminance
  (dark glyph on bright hue, light glyph on dark hue) — corrects every
  theme, dark ones unchanged where the hue is bright.
- [x] **B6 — sample-view loop region near-invisible on light.** The loop
  fill uses `primary_glow` (0.08 alpha on Arctic) so only the two edge
  lines read. Fix: a dedicated loop-fill alpha that stays visible on a
  light background.
- [x] **B7 — piano-roll toolbar tooltip gaps.** `-12 / +7 / +12`
  (transpose), `r4 / r8 / o1 / o2` (arp rate/octaves) and `g50 / g75`
  (gate) carry no tooltip while their neighbours do. Add them.
- [x] **B8 — light-aware CRT.** The CRT pass is hard-gated off on light
  themes (bright-pass/scanline/vignette constants assume a dark scene).
  Implement a light composite variant (subtle scanline via the theme
  scanline tint, no dark bloom halo / reduced vignette) so the toggle
  means something on Arctic Light — or keep the gate with a recorded
  reason.
- [x] **B9 — cable palette contrast on light.** reroute (#60c0ff),
  preview (#ffd060), cv (#a0e060) and midi (#e6b84a) are light-on-light
  on Arctic. Darkened by 0.55 on light themes (`cable_overlay.cpp`);
  dark themes keep the verbatim identity hues.

## Cosmetic (consistency)

- [x] **B10 — window-title casing.** `workspace` / `piano roll` / `midi`
  are lowercase while every other DAW window is uppercase
  (PATTERN / FX MIXER / SAMPLES / …). Normalize to WORKSPACE / PIANO
  ROLL / MIDI across Begin(), the VIEW menu, the dock builder and the
  focus calls. One-time layout reset for existing configs (VIEW → reset
  layout); noted in FIXES.md.
- [x] **B11 — FX MIXER empty state is a caveat, not a hint.** Empty rack
  shows only the "structural edits stop the transport" warning; add a
  "no FX channels — add one for delay/filter/reverb sends" empty hint.

## Recorded backend candidates (verify, fix if it fits the sweep)

- [x] **C1 — `play_from(order_pos)` honest transport primitive.** Added
  `ProjectSession::play_from` (seeds the engine order_pos through the
  existing `kTransportPlay` `aux_int`); the pattern ORDER list plays from
  a double-clicked entry (row/order-accurate). The piano-roll button
  stays "play from start" honestly (it has no per-order cursor); its
  stale "not wired" comment is corrected.

## Deliberate leaves (with reason)

- [ ] **C2 — EngineSnapshot sub-tick frame remainder — LEFT.** Precision-
  only: the recorded worst case is the live-MIDI-record quantise anchor
  reading up to one tick early right at a boundary. It touches the
  golden-tested RT engine snapshot for a gain no beta report has asked
  for (Stage 17's own note: "publish the remainder if beta reports
  drift"). Not worth the RT-path risk inside a UX polish sweep; the
  trigger to do it is a real drift report.
- [ ] **C3 — session-level preview-file API — LEFT.** The browser's
  process-lifetime decode cache is correct; its only cost is memory
  growing with the number of distinct files auditioned in one long
  session (no use-after-free, no wrong audio). Replacing it with a
  reclaimer-backed session preview is a backend refactor out of scale for
  a UX sweep; recorded for a sampler-lifecycle stage.
