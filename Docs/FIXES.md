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
- **Gate-length legato is pitch-blind-fixed** (`sequenceMidiTools.ts:
  199-217` extended each note only to the next note of the *same*
  pitch, stretching held notes across intervening pitches and leaving
  cross-pitch gaps unfilled): native `engine/sequence_ops.cpp`
  gate_length at ≥100% extends to the next later-starting note of any
  pitch (chord-mates share one gap); final notes fall back to plain
  scaling.
- **Transform results keep selection order** (web
  `applyVelocityCurve`/`adjustGate` returned notes re-sorted by start
  tick, breaking index correspondence with the caller's selection):
  native sequence transforms compute by time rank but return input
  order; the applier re-sorts once at republish.
- **Deterministic humanize/arp randomness** (web used bare
  `Math.random()` — irreproducible): native transforms take a
  caller-seeded `std::mt19937` and derive values from its
  standard-specified raw output, so a seed reproduces identically on
  every platform (pinned by exact-value tests in
  `tests/sequence_ops_test.cpp`).
- **New (native-only): pattern block operations** — the web clipboard
  is single-cell (`TrackerCanvas.tsx:510-524`); the native grid adds
  block selection, field-masked copy/cut/paste with edge clipping,
  transpose, and FT2-style effect-param interpolation (same command
  required at both endpoints, command copied onto filled rows), per
  the stage spec's exceed-the-thinness clause (`app/pattern_ops.cpp`).
- **New (native-only): export tail no longer replays the song** — the
  engine loops at song wrap and v1's export watched the one-shot
  `song_ended` flag at pull granularity, so the "tail" could keep
  sequencing from order 0 (timing-dependent). The transport now parks
  at the range end / song wrap (kTransportPlay's exclusive end bound,
  mirroring the web's break-before-schedule at
  `exportRenderOffline.ts:272`) with voices ringing out, and export
  polls per 128-frame block. The safety cap scales with render rate.
- **True-peak/LUFS measured, not approximated**
  (`exportPostProcess.ts:121-137` used 4× linear-interpolation "true
  peak" and 48 kHz-only hard-coded K-weighting): native normalization
  goes through libebur128 (BS.1770-4 polyphase true peak, per-rate
  filter recalculation) — conformance-passing rather than
  approximately right.
- **Equal-power fade shapes are native-only** (web `applyFade` was
  linear-only): the shape option is an exceed per the stage spec.
- **ID3v2 via lame, not hand-rolled** (`exportMetadata.ts:161` built
  ID3v2.3 frames by hand): native resolves lame's `id3tag_*` symbols
  optionally; when the loaded lame lacks them, MP3 tags are skipped —
  documented, never silently wrong.
- **Not ported (recorded)**: the web's `bypassCompressor` render
  option, RIFF cue-point chunks, BPM tags, the dither option, and
  multi-format batch renders (the "Everything" preset — one format
  per render natively). Recorded candidates if demand appears; the
  "Live Capture (debug)" preset is obsolete by design (it existed to
  work around the ScriptProcessorNode capture path, fixed natively).
- **Normalize scales down as well as up, to a target**
  (`WaveformEditor.tsx:278` silently refused any peak ≥ 0.999 and
  always aimed for full scale): the native op takes a peak target and
  applies `target/peak` in both directions, so hot samples can be
  pulled back. Silent selections refuse with a visible error instead
  of a silent no-op. Pinned by `tests/sample_ops_test.cpp`.
- **Destructive edits play exactly what they persist** (the web editor
  kept the un-quantised Float32 buffer resident while saving PCM16 —
  what played diverged from what a reload decoded): native ops encode
  the edited audio to PCM16 WAV (`io::encode_wav_pcm16`, the export
  writer's quantisation) and rebuild the resident buffer by decoding
  those exact bytes. One LSB of quantisation per op is the honest,
  reload-stable cost; the round-trip is pinned bit-exact in
  `tests/sample_ops_test.cpp`.
- **Equal-power fade shapes in the sample editor are native-only**
  (`WaveformEditor.tsx:251,260` were linear-only): same envelope
  formula as the export post chain; the linear grid matches the web
  exactly (fade-in starts at 0, fade-out ends one step above it).
- **DC removal is whole-sample only** (web `toolRemoveDC:314` honoured
  a selection): a partial DC correction introduces a step at the
  selection edge — a click. Recorded divergence, not an omission;
  range support returns if a real use case appears.
