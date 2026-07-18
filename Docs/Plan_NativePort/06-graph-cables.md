# 06 — Patch Graph and Cables

## Model

Ported concepts from `Source/federated-industries-main/src/lib/workspaceCableGraph.ts`
with the web's acknowledged hacks designed out.

- **Nodes** are workspace instruments: NTP plugins and pedals
  ([07-plugins-ntp.md](07-plugins-ntp.md)), external CLAP/VST3 plugins
  ([08-external-plugins.md](08-external-plugins.md)), the Module Player
  ([05-module-playback.md](05-module-playback.md)), and pseudo-nodes
  (Tracker Bus, Master In, Clock source/sink, Ext MIDI in/out —
  web: `workspaceTrackerBus.ts`, `workspaceMasterIn.ts`,
  `workspaceBuiltinMidiPlugins.ts`).
- **Ports** are typed jacks: `audio | sidechain | cv | gate | midi`
  (compat matrix ported from `pluginTypes.ts:94-104`). Every port has a
  **stable string ID**; indices are a render detail only (fixes #14/#15 —
  the web kept a legacy positional model in parallel,
  `pluginInsAdapter.ts:94-113`, and its drag commit passed only
  jackIndex, `CableOverlay.tsx:322`).
- **Cables** are one-way source-output → dest-input edges with the web's
  two modes: `tap` (fan-out alongside the master route) and `reroute`
  (suppresses the master route) — `workspaceCableGraph.ts:63`.
- Persistence: cables + window snapshots in the FTRK workspace block
  ([10-formats-io.md](10-formats-io.md)).

## Evaluation (audio thread)

The graph compiles to a block-ordered schedule evaluated per 128-frame
block ([03-audio-backend.md](03-audio-backend.md#block-model)):

- **audio/sidechain** — float block summing/fan-out.
- **cv** — float block written to a parameter smoother on the dest node.
- **gate** — block-accurate edge events (frame offset within block),
  replacing the web's ~60Hz analyser-RMS polling
  (`workspaceCableGraph.ts:107-146`, fix #11).
- **midi** — timestamped event lists routed through the graph, not a
  side-channel bus.

<a name="feedback"></a>
**Feedback**: cycles are legal. Any cycle is broken at one edge by an
implicit one-block delay (standard modular-synth semantics; 128 frames
at 48kHz ≈ 2.7ms). This deletes the web's self-feedback ban
(`CableOverlay.tsx:319`, fix #16) and makes feedback patching a feature
instead of an error. The UI marks the delayed edge subtly on the cable.

Failed connections (kind mismatch, unresolved endpoint) give visible UI
feedback — a shake + status line, not `console.warn`
(`workspaceCableGraph.ts:284-388`, fix #13). Strict validation happens
at connect time; the audio thread only ever sees valid schedules
(swapped in RCU-style — [02-architecture.md](02-architecture.md)).

## Rendering and interaction (UI thread)

- Cables draw into ImGui's background/foreground draw lists as polylines
  from the verlet rope sim — a direct port of `cablePhysics.ts`
  (verlet integration, slack/gravity/damping/iterations, live-tunable
  via the web's `cableSettings.ts` equivalents). The sim is cheap; it
  runs inline on the UI thread. The web's worker + inline duplication
  and its one-frame lag die here.
- Jack anchor positions come from the app's own window registry
  ([09-windows-ui.md](09-windows-ui.md)) — ImGui gives exact widget
  rects the same frame. The web's per-frame DOM queries, phantom
  windows, and minimised jack-strips (`CableOverlay.tsx:83,218`,
  `InstrumentWindowPhantom.tsx`) all exist to fake this and are not
  ported (fix #12).
- Interaction parity: drag from an output jack, preview cable follows
  cursor, drop on a compatible input (highlighted while dragging);
  right-click deletes; midpoint chip toggles tap/reroute. Kind drives
  cable colour/dash exactly as the web (`CableOverlay.tsx:406-427`).
- Minimised windows keep a compact jack rail in the collapsed titlebar
  rect (the *behaviour* survives; the DOM-anchor machinery does not).

## File map

- `src/graph/graph_model.{h,cpp}` — nodes, ports, cables, validation
- `src/graph/graph_compile.{h,cpp}` — schedule build, cycle/delay
  placement
- `src/audio/graph_runner.{h,cpp}` — block evaluation (shared with 03)
- `src/ui/cable_overlay.{h,cpp}` — verlet sim + draw-list rendering,
  drag interaction
- `src/graph/pseudo_nodes/*.{h,cpp}` — tracker bus, master in, clock,
  ext MIDI
