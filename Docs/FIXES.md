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
  Revisited (Stage 20): REVERB now carries a `mode` parameter — 0
  keeps the Freeverb bank (default, unchanged), 1 is convolution
  against the web's synthetic impulse (`lib/reverbIR.ts`
  construction: pre-delay silence + exp(−3t/decay) noise, 0.97
  right-channel decorrelation) through the shared partitioned-FFT
  engine (`audio/convolution_engine.h`). Two documented deviations:
  fixed-seed noise (determinism) and unit-energy normalisation
  (standing in for ConvolverNode.normalize). Impulse rebuilds
  allocate, so mode/decay/preDelay changes in convolution mode
  republish the rack structurally; wet/dry stay live. Pinned by
  `tests/fx_chain_test.cpp`.
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
  Revisited (Stage 20): the cap is gone — impulses above 256 taps
  run through the uniform partitioned-FFT engine
  (`audio/convolution_engine.h`, 128-frame partitions, zero added
  latency, null-tested against direct FIR at 1e-5); at or below 256
  taps direct FIR remains (cheaper there). The only bound left is a
  10 s-at-device-rate memory guard, refused at load with a collected
  error — never truncated. Pinned by `tests/convolution_test.cpp`
  and the >2048-tap fixture in `tests/ntp_test.cpp`.
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
- **Effect→MIDI was dormant and namespace-mismatched in the web**
  (`trackerEffectToMidi.ts` mapEffectToMidi and
  `getTrackerBusControls` had zero callers, and the mapper speaks IT
  effect letters while both pattern models store MOD nibbles — it
  could never have fired): natively wired for real
  (`audio/effect_midi.{h,cpp}`) with a documented MOD-nibble bridge
  (0x1→Fxx, 0x2→Exx, 0x3→Gxx, 0x5/0x6/0xA→Dxx halves, 0x8→Xxx,
  0xC→Mxx, EAx/EBx fine slides, ECx→noteOff-at-tick), byte-exact to
  the web mapper's math.
- **master.midi.in reaches something** (the web dispatched inbound
  bus MIDI to an `onInboundMidi` handler nothing ever installed):
  natively delivered into an engine SPSC ring
  (`AudioEngine::poll_bus_midi_in`) — the pattern-record surface.
- **MIDI feedback via one-block delay, not hop counting** (web
  MidiBus capped propagation at maxHops 32/256): cycle-closing midi
  edges read the previous block, same rule as audio/CV feedback;
  per-block lists are bounded with counted overflow.
- **Txx→BPM emits no wire event** (the web's "bpm" midi event kind
  had no byte form and no consumer): tempo reaches hardware as the
  clock-rate change through the PLL 24 PPQN clock thread.
- **No implicit midi-thru/midi-out adapter ports** (web adapters
  created them implicitly): cables addressed to them stay dormant
  with lossless round-trip, matching the strict-port model.
- **MIDI step entry follows the pattern cursor** (`Tracker.tsx:1927`
  kept a separate `midiEnterRow` counter with its own RESET button,
  ignoring the editor cursor): inbound notes in enter mode write at
  the pattern cursor through the exact keyboard-entry path and advance
  it the same way (`app/midi_record.cpp` `enter_note_cell`, shared
  with `ui/pattern_view.cpp`) — locked in `Plan_PostV1/06`.
- **Velocity→volume OFF leaves the volume column alone**
  (`Tracker.tsx:1915` wrote `0xFF` in enter/record when the toggle was
  off, clearing any volume already in the cell): keyboard entry never
  touches the volume column, and step entry is keyboard-parity, so
  the off state keeps the cell's value; ON maps round(velocity/127·64)
  exactly as the web did.
- **Live record quantises from event timestamps**
  (`Tracker.tsx:1934` wrote at whatever `ps.row` the UI thread
  happened to observe): cabled `master.midi.in` events carry absolute
  stream-frame stamps and quantise to the nearest row against the
  transport snapshot — second half of a row rounds UP to the next row
  (`app/midi_record.cpp` `quantise_record_row`); untimed device notes
  are stamped at the latest snapshot and take the same path.
- **Local API is v1.2 — additive, typed, never silent** (web
  `trackerLocalApiSchema.ts:104-107` fix-list #5): sample binary
  upload lands through the standard decode path (the web dropped it);
  workspace discovery exists (`getWorkspace` enumerates nodes, typed
  ports, cables); bogus IDs return typed errors
  (`notFound`/`outOfBounds`/`invalidOp`/`unsupported`) and store
  nothing (the web silently stored ghost workspace ids). Web ops with
  no native session surface yet (pattern/order-list structure,
  channel reshape, seq layer add/remove) return typed `unsupported`
  rather than pretending; wire cap raised 1 MiB → 64 MiB so uploads
  fit; queue bounds replace rate limiting (`rateLimited` on
  overflow); mid-batch I/O failures report the failing index +
  appliedBeforeFailure (a case the web surface never expressed).
  Full delta in the stage ledger.
- **User-assignable zones require slotId + fallbackFile** (the web
  only "strongly recommended" the fallback and could play silence on
  a fresh install): strict load with collected errors
  (`plugins/ntp_loader.cpp`).
- **sliceMap autoDetect — real transient + marker slicing** (Stage 27;
  was parked): `"transients"` no longer normalises to `grid:16` (web
  v4.1.0's silent runtime fallback) and `"markers"` is no longer
  refused. Both resolve for real in the post-decode expansion
  (`plugins/ntp_loader.cpp`), filling `map.slices` the runtime already
  reads (it never distinguishes derived from authored slices). The
  early validation now *accepts* `transients`/`markers`/`grid:N` and
  resolves them at expansion, rather than rewriting them at parse.
  - **`"transients"`** runs a deterministic spectral-flux onset
    detector (`audio/onset_detect.{h,cpp}`) over the mono-summed decoded
    buffer. STFT with a Hann window, ~21 ms window rounded to a power of
    two at the source rate (1024/512 hop at 48 kHz), 50 % overlap;
    per-frame magnitude spectrum via the project pffft wrapper's public
    API only — `|X[k]|² = X·conj(X)`, and `conj(X)` is the transform of
    the circularly time-reversed frame, so `forward + forward +
    convolve_accumulate` yields the power spectrum without touching
    pffft's opaque bin layout (no change to `dsp_fft`). Half-wave-
    rectified flux, adaptive threshold (local mean + 1.6·std over a
    ±100 ms window) plus a level-invariant magnitude floor, peak-pick
    with a 30 ms minimum inter-onset gap. Deterministic (no RNG; total-
    order tie-break by flux then earlier frame). Decisions: the detector
    emits only interior transient offsets (frame 0 has no preceding STFT
    frame, so it is never an onset) and the loader forces slice 0 to
    start at frame 0 — the head is always addressable, no audio dropped
    ("first onset forced to 0", not "dropped"); offsets are reported at
    the analysis frame *centre* (`t·hop + window/2`), landing within one
    hop of the true transient. Onsets are capped to the note ceiling
    (≤ 92 slices, `kSliceBaseNote`); when more transients exist the
    strongest by flux are kept. A silent, DC, or untransient source
    yields a single whole-sample slice plus a collected *warning* — an
    honest fallback, never a silent mis-slice.
  - **`"markers"`** parses the WAV `cue ` chunk from the source's raw
    archive bytes (cue points live in the container, not the decoded
    floats), converting each `dwSampleOffset` (source-rate frames) to a
    slice boundary. A non-WAV source or one with no cue points is a
    collected *error* (not a whole-sample fallback): "markers" is an
    explicit request the loader cannot satisfy without cues, so the
    strict-load philosophy applies — the author is told exactly why.
  - **Deferred — slice preview overlay**: drawing slice boundaries on a
    sampler node's source was scoped for the stage but has no natural
    home in native v1. `ui/ntp_ui.cpp` renders only declarative controls
    (knob/slider/xy/meter/envelope/sprite — no sampler-source waveform),
    and `ui/sample_view.cpp` edits the tracker's own session sample
    slots, not an NTP sampler node's slice source. A boundary overlay
    would first need a waveform view for NTP sampler nodes (a new panel
    or control kind) — significant surgery — so it is deferred rather
    than bolted on.
- **New (native-only): POVR raw carry no longer swallows trailing
  blocks** — the verbatim passthrough read to EOF, duplicating
  PPRS/XPLG inside the raw copy on re-save; the carry is bounded by
  the next block offset.
- **New (native-only): stale POVR cleared on project switch** —
  new_project()/module imports previously kept the prior project's
  carried POVR, re-emitting foreign overrides on save.
- **New (native-only): zone round-robin advance was allocating on the
  audio thread** (a per-trigger std::vector<bool>, caught by the
  debug allocator once RR groups went live): preallocated scratch,
  shared with the slice RR path.
- **Envelope editor edits the format's truth** (the web editor moved
  ADSR *parameters* by naming convention): native drags write the
  envelope node's stage array itself — target/time per stage, the
  thing FTRK/NTP actually store. Recorded gap: PLGB re-embeds the
  original archive on save, so stage edits are session-live only
  (save→load reverts) and plugin-wide (the manifest is shared per
  plugin id). Persisting per-instance stage overrides is the
  recorded revisit.
- **Sprite animations are declarative** (web sheets registered
  animations from driver code with chain()/tint): NTP declares
  animations per sprite control (named frame lists, fps default 10,
  loop flag) with strict collected validation; chain() and tint are
  not ported (no consumer in the web corpus; ntp-convert unchanged).
- **Light-theme legibility polish** (native-only contrast fixes over the
  web's dark-canvas assumptions): the pattern channel pill drew its hex in
  the window background colour, so bright hues (lime/cyan/yellow) rendered
  near-white text on light themes — now the glyph colour follows the
  hue's own luminance (`pattern_view.cpp` `pill_text_color`), fixing every
  theme. The sample-view loop region filled with `primary_glow` (0.08
  alpha on Arctic), invisible over the waveform — a firmer light-theme
  fill alpha now marks it (dark themes keep `primary_glow` exactly). The
  cable palette (`cable_overlay.cpp`, previously "identity, kept
  verbatim") darkens its bright hues by 0.55 on light themes so
  reroute/preview/cv/midi cables are not light-on-light.
- **CRT pass has a light-theme variant** (the shader's bright-pass/
  scanline/vignette constants assume a dark scene): originally gated off
  on light themes with an explanation. The polish sweep implemented the
  recorded light variant — `crt_pass.cpp`'s composite shader branches on
  a `u_light` uniform: the light path skips the additive glow bloom
  entirely (it only washes out a bright scene), tints the darker scanline
  phase toward the theme primary (`u_scanline`, the web's
  `rgba(51,136,170,0.02)` scaled up for legibility) and keeps a faint
  vignette; the glow chain is skipped outright on light themes. The
  SETTINGS toggle is now live on every theme. Dark-theme rendering is
  byte-for-byte unchanged (the light branch is a separate shader path;
  interactive alphas and the toward-black scalings still reduce to the
  old values on near-black palettes).
- **IT compressed samples: 2.14 vs 2.15 variant is per-sample, not
  per-file** (`itImporter.ts` and the first native cut both keyed the
  IT2.15 double-delta integrator off the file header `cmwt >= 0x0215`,
  applying one choice to every sample): real files mix the two, and the
  variant is carried per sample by the cvt "delta" bit (0x04) — exactly
  as Impulse Tracker and libopenmpt (`ITSample::GetSampleFormat`,
  `SampleIO::IT214`/`IT215`) decide it. The owner file
  `02fd_-_lumifluidity.it` (cmwt=0x0215, 35 samples, ~half cvt=0x01)
  imported as noise on every single-delta slot; forcing double-delta on
  single-delta data ramps into garbage (first-difference RMS ≈ signal
  RMS). `io/import/it_importer.cpp` now derives `it215` from each
  sample's cvt bit. The decompressor itself was already correct
  (cross-checked bit-exact against libopenmpt's ITDecompression on the
  real file); only the variant selector was wrong. Covered by
  `tests/golden/nttest_compressed.it` (8/16-bit × 2.14/2.15, header
  cmwt=0x0214 so a cmwt-keyed decoder fails the test) and a guarded
  owner-file harness in `tests/it_import_test.cpp`.
- **IT dropped samples and their pattern references are reported, not
  silent** (fix-don't-retain #2 extended): the importer kept the first
  31 samples and stopped; slots 32+ vanished with no trace. It now
  tallies every valid sample past the engine's `kMaxSamples` and warns
  ("samples 32-35 dropped — native slot limit (31)"), and separately
  warns when pattern cells (directly, or via the instrument→sample
  remap) still name a dropped slot, so silent notes are explained.
- **IT uncompressed delta-PCM and stereo handled honestly**
  (native-only correctness, adjacent to the above): the cvt delta bit
  on an *uncompressed* sample means delta-PCM — now integrated at the
  native width instead of read as raw noise. IT stereo is stored split
  (left channel first, contiguous), not interleaved; the engine's
  samples are mono, so import keeps the left channel and warns rather
  than averaging a mis-indexed interleave. No stereo/compressed sample
  exists in the owner corpus to fixture, so this path is verified by
  construction and the uncompressed-delta path by the fixture's slot 4.
- **Window titles are uniformly upper-case** (native-only consistency):
  the WORKSPACE / PIANO ROLL / MIDI windows were lower-case while every
  other DAW window was upper-case. Normalized across the Begin() titles,
  the VIEW menu, the dock builder and the focus calls. A config saved by
  an older build docks those three windows by their old ids, so the first
  launch after this change leaves them floating until VIEW → reset layout
  (one-time; fresh configs are unaffected).
- **deletePattern reindexes instead of leaving id gaps** (the web
  programmatic `deletePattern`, `trackerLocalApi.ts:408-413`, filtered
  `patterns` and `orderList` by `id !== patternId` and left the remaining
  ids un-renumbered, so `id !== array index` afterward — which the
  playback engine reads by direct array subscript
  (`trackerEngine.ts:478,528`), silently breaking every later pattern
  lookup). The native engine has the same index==id contract
  (`tracker_engine.cpp` `pattern_by_id`, and `seq_patterns`/`fx_patterns`
  indexed by the order-list value), so `ProjectSession::delete_pattern`
  reindexes `patterns`, the parallel `seq_patterns`/`fx_patterns`, and the
  `order_list` together: refs to the removed pattern drop, higher refs
  shift down one, and remaining ids renumber to their new index. The
  "never empty the order list" fallback matches the web's `safeOrder`.
- **order_remove refuses emptying rather than blanking the pattern** (the
  web `handleOrderRemove`, `Tracker.tsx:2107-2119`, on the last order
  entry did NOT remove it — it cleared the referenced pattern to a fresh
  blank and returned): the native `order_remove` simply refuses (returns
  false, `error()` set) when one entry remains, keeping "remove" a pure
  structural op. Clearing a pattern is a separate concern.
- **Pattern add/delete/resize + order-list edits exist natively** (the
  web UI could only ever append patterns — no delete/reorder affordance —
  and structure editing lived in the programmatic local API): the native
  session grows `create_pattern` / `delete_pattern` / `resize_pattern`
  (1..256 rows, web-parity pad/truncate + sequence-note clamp) and
  `order_insert` / `order_remove` / `order_move` / `order_set` /
  `set_order_list`, surfaced in the PATTERN window's ORDER column
  (+ / − / ↑ / ↓ and a modulo repoint cycle, matching the web renderer's
  order controls and `handlePatternChange`). All are structural
  (stop + republish) and clear undo, the same discipline the sequence
  edits use. The six matching Local API ops (createPattern, deletePattern,
  resizePattern, insertOrderAt, removeOrderAt, setOrderList) move from
  `unsupported` to real with typed-error validation.
- **Live song tempo is editable** (there was no way to change BPM/speed
  from the UI — the transport only displayed them): the transport bar
  now hosts BPM (20-255) and SPD (1-31) drag/type fields;
  `ProjectSession::set_tempo` writes the project set point and, via a
  new `AudioEngine` `kSetTempo` command, updates the running transport's
  play state so a mid-playback change is audible from the next tick.
  Requested by the owner during this fix stage.
- **Envelope stage edits are per-instance and persistent** (closes the
  Stage 20 recorded gap above): NTP envelope-editor drags previously
  mutated the shared manifest (`plugins/ntp_voices` `set_env_stage`),
  so an edit reached every instance of the same plugin id, and PLGB
  re-embedding the original archive meant the edit reverted on
  save→load. The web editor moved ADSR params on a per-node basis with
  no cross-instance leakage, so the plugin-wide scope was a native
  regression, not web parity. Now each `NtpInstance` holds an override
  table and each envelope `NodeRuntime` its own stage copy
  (`ntp_graph` `env_stages_`); `set_env_stage` writes only this
  instance's copies, leaving the manifest authored. The edits persist
  in a new additive FTRK block, **XINS** (v15, `Docs/ftrk-format.md`) —
  the writer emits from live instances, the reader replays into them
  after instantiation; files without the block load unchanged.
- **native_stage ABI state persists** (Stage 21 recorded gap): the
  frozen `ntp_stage_abi.h` `state_save`/`state_load` were exercised by
  tests but never written to disk — a stage's internal state was
  session-live only. XINS (above) now carries each native_stage node's
  opaque chunk keyed by node id; the reader restores it through
  `set_native_stage_state` after the instance instantiates. The ABI
  promises stage authors that `state_save` (main thread) never overlaps
  `process()`, but the workspace graph is processed on the audio thread
  on every block whenever the device is open — independent of the
  transport — so there is no session-thread window to read a live
  stage's state while the device pulls. The save path therefore only
  calls `state_save` when the device is idle (offline export, or before
  the first bundle publishes) and otherwise emits the last quiescent
  capture cached at load / device-idle save (`project_session.cpp`).
  Consequence, recorded not hidden: internal state a stage evolves
  purely inside `process()` mid-session is persisted at its last
  quiescent value, not continuously; an RT-coordinated capture (post a
  quiesce command, snapshot-ack, then save) is the future path if a
  real stage needs it. The frozen ABI contract is never violated.
- **Record quantiser anchors on the sub-tick frame edge** (the web
  recorded MIDI input at whatever row the UI thread happened to observe
  — strictly coarser): `EngineSnapshot` now publishes
  `tick_frame_remainder` (frames elapsed since the current tick's edge
  at publish), so `app/midi_record`'s quantiser recovers the exact
  frame the (row, tick) began instead of assuming the publish landed on
  the boundary. This removes the up-to-one-tick early read near row
  boundaries recorded at Stage 17; the change is snapshot-additive, so
  the golden RT render path is untouched.
- **Asset browser scope (deliberate)** — the web exposed project and
  asset management through browser file APIs and IndexedDB. Stage 28's
  PROJECTS and LIBRARY windows (`ui/projects_view`, `ui/library_view`)
  deliberately narrow this to the owner's scope guard: a browser over
  real directories, no database and no tagging beyond favourites.
  Concretely: (a) library roots are added by an in-app path field, not
  an OS file picker — the whole native port avoids in-tree OS dialogs
  (`ui/sample_browser_view` set the precedent); (b) the LIBRARY window
  lists only what it can act on — samples (auditioned through the Stage
  25 `preview_file` path) and NTP archives (installed via
  `load_plugin_file`); (c) user presets are NOT browsed here — the
  preset bank stores them per-plugin as JSON in the config directory
  (`plugins/preset_bank`, the web's IndexedDB store) and they apply to a
  live instance from the plugin panel, so a loose-file listing would
  carry no actionable verb. The recursive listing is bounded (depth and
  entry caps) and cached per root-set — never an unbounded synchronous
  walk each frame.
