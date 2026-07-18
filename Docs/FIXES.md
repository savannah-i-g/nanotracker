# Fixes Ledger — Deliberate Divergences from the Web App

Every entry records: what the web app did (with `file:line` in
`Source/federated-industries-main/`), what the native app does instead,
and why. Seeded from the 16-item fix-don't-retain list in
`Plan_NativePort/01-context.md`; entries move here with implementation
detail as each fix lands.

## Landed

- **#1 — XM version validation** (`xmImporter.ts:392` ignored the
  version field): `io/import/xm_importer.cpp` checks for 0x0104 and
  loads non-standard files best-effort with a loud warning. The
  relNote base-note sign (`xmImporter.ts:330`'s historical bug) is
  pinned by `tests/xm_import_test.cpp`.
- **#2 — no silent effect degradation** (`xmImporter.ts:58` zeroed
  unknown effects silently; `s3mImporter.ts:47`, `itImporter.ts:62,82`
  dropped/approximated silently): all four native importers count
  approximations and drops and surface them in ImportResults warnings.
- **#7 — no ScriptProcessorNode capture path**
  (`trackerAudio.ts:1055-1088` captured through a deprecated
  ScriptProcessorNode): `io/export_render.cpp` renders offline through
  the same engine the device drives — the project serialises to FTRK,
  loads into a fresh offline session, and the transport is clocked by
  sample count until the order list wraps plus a tail. What exports is
  exactly what persists.
- **#9 — loop points in played-buffer frames**
  (`trackerAudio.ts:660-668` divided source-rate loop points at play
  time): `audio/audio_engine.cpp` converts loop points to resident-
  buffer frames at voice trigger; the sampler never mixes rate
  domains. FTRK fields keep source-rate semantics for compatibility.
- **#12 (part) — no phantom windows**: the pattern editor and window
  registry take anchors from ImGui layout directly; the cable stage
  builds on this.
- **Reverb topology divergence (deliberate)** — the web REVERB module
  (`trackerFxModules/Reverb.ts`) is convolution against a synthetic
  impulse because WebAudio ships a convolver; the native module
  (`audio/fx_chain.cpp`) is a Freeverb-style comb/allpass bank with the
  same parameters (wet/dry/decay/preDelay). Same control surface,
  appropriate native cost; convolution parity can be revisited with a
  partitioned-FFT engine if character differences matter in practice.
- **New (native-only): IT decompressor corrupt-escape guard** — the
  web decompressor (`itImporter.ts` BitReader paths) allows corrupt
  escape sequences to drive the bit width to 0 (a negative shift,
  masked by JS semantics). The native port clamps the width; caught by
  static analysis and recorded here because the web original shares
  the latent defect.
- **#11 — block-accurate gate signals** (`workspaceCableGraph.ts:107-146`
  polled an analyser at ~60Hz RAF cadence to fake gate edges):
  `audio/graph_runner.cpp` derives gate events from the source signal
  per block with exact frame offsets (0.5 rise / 0.4 release
  hysteresis). Verified to the frame in `tests/graph_test.cpp`.
- **#12 — jack anchors from live layout** (`CableOverlay.tsx:83,218`
  ran per-frame DOM queries; `InstrumentWindowPhantom.tsx` and the
  minimised jack-strip DOM existed only to keep anchors alive):
  `ui/workspace_view.cpp` records every jack's screen position from
  ImGui layout the same frame it draws; collapsed windows draw a
  compact strip into the foreground list. No phantom machinery exists
  to port.
- **#13 — visible connection feedback** (`workspaceCableGraph.ts:284-388`
  dropped failed connections with `console.warn`): `graph_model.h`
  returns a typed `ConnectResult`; the workspace status line shows the
  reason, and while dragging, input jacks glow green/red by
  compatibility. Load-time cable drops surface as project warnings.
- **#14/#15 — single typed port model with stable IDs**
  (`pluginInsAdapter.ts:94-113` kept legacy positional `inputs[]` /
  `outputs[]` parallel to typed ports; `CableOverlay.tsx:322` committed
  drags by jackIndex): the native model has one port list per
  direction, addressed by stable string ID everywhere; indices are a
  render detail. WPBR adoption maps legacy jackIndex snapshots through
  the web's historical layouts once, at load.
- **#16 — feedback cables legal via one-block delay**
  (`CableOverlay.tsx:319` banned self-feedback outright as a stopgap):
  `graph_compile.cpp` marks every cycle-closing edge as delayed one
  block (128 frames ≈ 2.7ms at 48kHz, standard modular semantics).
  Self-patching a node is now a feature; ring stability is unit-tested.
- **Reroute suppression is per source jack (deliberate)** — the web
  suppressed the master route per *instrument*
  (`workspaceCableGraph.ts:227-237`), which made reroute cables from
  the tracker bus a silent no-op (the bus has no master route;
  `workspaceTrackerBus.ts:19-21`). Natively a reroute cable from a bus
  channel jack suppresses that channel's dry path, and one from the
  module player suppresses its direct mix — reroute means the same
  thing everywhere. Verified by capture (rerouted channel silent when
  its cable dead-ends).
- **#10 — strict plugin validation, no degraded mode**
  (`pluginInstrumentGraph.ts:562` fell back to a half-working
  "degraded" state on schema problems): `plugins/ntp_loader.cpp`
  either loads a plugin completely or refuses it with the full
  collected error list. Worklet nodes and `native_stage` reject with
  messages that say what to do instead.
- **#15 (completed) — single plugin schema version** (web
  `pluginRegistry.ts:82` carried v1-v5 migration debt): NTP has
  exactly one schema (`"ntp": 1`); `tools/ntp-convert` maps web
  manifests across — legacy oscillator/envelope/filter shorthand
  synthesises into the graph form, v2 single-target mod routes
  normalise, and everything unconvertible (worklet DSP, webview UIs,
  splitter/merger channel routing) is reported explicitly.
- **Convolver capped at 2048 taps (deliberate)** — the web convolver
  is WebAudio's ConvolverNode (FFT under the hood); the native node
  is direct-form FIR with the impulse truncated to ~43ms at 48kHz
  (`plugins/ntp_graph.h kMaxImpulseFrames`). Ambience/cab-style
  impulses survive; long reverb tails wait on a partitioned-FFT
  engine (post-v1 backlog). Load emits no warning today because the
  cap is structural, not data-dependent — revisit if authors hit it.
- **Mod routes evaluate at block rate (deliberate)** — the web routed
  modulation through live AudioNodes (audio-rate); NTP mod routes are
  k-rate per 128-frame block reading the source's previous block,
  with per-target slew smoothing. Audio-rate paths still exist via
  `toParam` connections (FM et al) — same split WebAudio had between
  param automation and audio-rate connections in practice.
- **New (native-only): debug allocator covers nothrow new/delete** —
  the RT-check allocation hooks originally overrode only the throwing
  operator new/delete forms, so a library nothrow allocation
  (libstdc++ `stable_sort` temporary buffer) paired the default
  nothrow new with our `free()` — an alloc/dealloc mismatch flagged by
  ASan and a gap in the RT-thread check. All standard variants now
  route through one malloc/free path (`rt/rt_assert.cpp`).
