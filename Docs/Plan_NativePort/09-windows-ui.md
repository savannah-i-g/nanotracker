# 09 — Windows, UI, Visual Identity

## Window system

Dear ImGui docking branch, hybrid feel (locked decision 2,
[00-index.md](00-index.md)):

- **Instrument/patchbay windows: always free-floating**
  (`ImGuiWindowFlags_NoDocking`) — jacks and cables are meaningless in a
  tab bar. This preserves the site's defining patchbay feel.
- **Editor surfaces** (pattern grid, sample editor, piano roll, mixers):
  dockable, so large-screen users can build layouts the web never
  offered.

Behaviours mapped from the site's manager (web:
`TrackerWindow.tsx`, `InstrumentWindow.tsx`,
`InstrumentWindowManager.tsx`, `windowZOrder.ts`,
`trackerWindowLayout.ts`):

| Site behaviour | Native mapping |
| --- | --- |
| Raise on any mousedown, monotonic z-counter | ImGui-native focus/z behaviour |
| Titlebar drag, corner resize, min/max/aspect constraints | ImGui window flags + size-constraint callbacks |
| Minimise to titlebar with live jack rail | Collapsed ImGui window; jack rail drawn in the collapsed rect |
| Phantom windows keeping cable anchors after close | Not ported — anchors come from the window registry (fix #12) |
| Viewport clamping with squish/hysteresis | Window registry clamps to main viewport |
| Layout persisted per instrument in the project | Same, FTRK workspace block ([10-formats-io.md](10-formats-io.md)) |

Persistence ownership is single-source: `io.IniFilename = nullptr`; all
window placement is driven from the project block via
`SetNextWindowPos/Size`. imgui.ini and the project never fight.

The **window registry** (`src/ui/window_registry`) is the one place that
knows every app window's rect, z, collapsed state, and jack anchor
rects. The cable overlay reads it directly
([06-graph-cables.md](06-graph-cables.md)); the FTRK layout block
serialises from it.

Multi-viewport (dragging windows outside the OS window) is off in v1 —
the docking branch keeps it a flag-flip away; likely the first
post-release request for multi-monitor patchbays.

DPI: content-scale handling (fonts re-rasterised on scale change) gets a
dedicated task in the platform layer. Fractional scaling on Wayland+GLFW
is a live upstream defect (blurry rendering, imgui#7433 — Research/06);
posture: integer-scale rendering with font re-rasterisation, and
`GLFW_PLATFORM_X11` documented as the user-facing escape hatch (plugin
editors are X11-bound anyway).

## Visual identity

Ported faithfully from the web (`src/styles/tracker.css`, `theme.css`,
`trackerRenderer.ts`):

- **Font**: Kode Mono everywhere (SIL OFL 1.1 — bundled with OFL text in
  `assets/`).
- **Themes**: four phosphor palettes — amber (default, `#ffaa00` on
  `#1a1000`), green (`#33ff00`/`#001a00`), blue (`#00aaff`/`#000d1a`),
  red (`#ff0a0a`/`#120000`) — mapped into ImGui style + our own draw
  palette; dim/glow variants as in `trackerRenderer.ts:81-84`.
- **Per-channel hue palette** (`trackerRenderer.ts:22`) and yellow
  playhead in the pattern grid; user channel colors honoured
  (`TrackerProject.channelColors`).
- **CRT pass**: ImGui renders into an offscreen FBO; one post pass does
  scanlines/curvature, a small blur chain does glow. Intensity is
  user-scalable and defaults modest — full-strength scanlines fight
  text legibility, and legibility is the product.

## Views to port

Each web view becomes an ImGui window module (decomposing the
`Tracker.tsx` god component per the doctrine):

- Pattern editor + order list (`TrackerCanvas.tsx`/`trackerRenderer.ts`
  → `src/ui/pattern_view`): draw-list grid, cell cursor/entry model,
  order strip.
- Instrument editor (`InstrumentWindow.tsx`, 76KB → split into
  `src/ui/instrument_view` + jack rail component shared with graph UI).
- Sample/waveform editor (`WaveformEditor.tsx` → `src/ui/sample_view`).
- Piano-roll sequence editor + on-screen keyboard
  (`SequencePianoRollEditor.tsx` → `src/ui/seq_view`).
- FX mixer and volume mixer panels (`TrackerFxMixerPanel.tsx`,
  `TrackerVolMixPanel.tsx` → `src/ui/mixer_views`).
- Sample browser, settings, export, history, help, licence windows.

Keyboard model: the tracker's editing keys (note entry rows, octave
switch, edit step, hex entry) are defined in one keymap module,
user-remappable via settings ([10-formats-io.md](10-formats-io.md)).

## File map

- `src/ui/window_registry.{h,cpp}`, `src/ui/theme.{h,cpp}`,
  `src/ui/crt_pass.{h,cpp}`
- `src/ui/pattern_view.{h,cpp}`, `instrument_view/`, `sample_view.{h,cpp}`,
  `seq_view.{h,cpp}`, `mixer_views.{h,cpp}`
- `src/ui/keymap.{h,cpp}`
- `assets/fonts/KodeMono-*.ttf` + `assets/fonts/OFL.txt`
