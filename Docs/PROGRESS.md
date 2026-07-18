# Progress Log

One entry per stage (or notable milestone), newest first. Format:
date — stage — what landed — verification — backup filename.

## 2026-07-18 — Stage 14 closed — Pattern + piano-roll editing depth

- Landed: **undo grouping** (`app/undo` begin/end_group + RAII
  UndoGroup; nested groups commit at the outermost close). **Pattern
  block operations** (`app/pattern_ops`): sub-column-aware
  CellSelection, field-masked CellClipboard, copy/cut/paste with edge
  clipping, clear, transpose (clamped, note-off/empty untouched),
  FT2-style interpolate (volume, or effect-param with matching
  commands) — every op one undo entry through ProjectSession.
  **Pattern grid UI** (`ui/pattern_view`): shift+arrow / mouse-drag
  block selection with sub-column granularity, theme-glow highlight,
  Ctrl+C/X/V, Ctrl+I, block Delete, Alt+F1-F4 transpose, right-click
  context menu listing every operation. **Sequence transforms**
  (`engine/sequence_ops`, pure + platform-deterministic under a
  caller-seeded mt19937): quantize, humanize, transpose, reverse,
  invert, arpeggiate, velocity curves, gate length — input-order
  contract so selections survive transforms. **Piano-roll UI**
  (`ui/piano_roll_view`): rubber-band + shift-click multi-select,
  selection body-drag (move) and right-edge drag (resize), Ctrl+C/V
  with scroll-anchored paste, the full web toolbox as one strip; all
  batch edits flow through the new
  `ProjectSession::seq_replace_notes` (one stop→swap→publish).
  Fold-ins: INSTRUMENTS source picker (sample/plugin/workspace combos
  over the catalogue and live plugin nodes), ballistic shell meters
  (instant attack, exponential release). Help window documents the
  new keymaps. Four FIXES.md entries (gate-length pitch-blindness
  fixed, selection-order preservation, deterministic randomness,
  native-only block ops).
- Verification: 51/51 both trees (10 new unit tests: block-op
  round-trips, undo grouping, exact-value transform outputs with
  seeded rng); clang-tidy clean on all touched files; input-script
  runs with pixel-checked screenshots — block selection highlight
  with field trimming visible, paste at row 8 CH3 verified, piano
  roll +12 transpose moved notes exactly 12 rows with selection
  following, copy/paste/move/resize/escape all captured; CI green on
  both platforms.
- Backup: `NanoTracker_stage14_2026-07-18.tar.gz`; git tag `stage-14`.

## 2026-07-18 — Stage 13 closed — Repo, CI, Windows beta (post-v1 opens)

- Landed: **public repository** — `git init` in Output/, publish scrub
  (README with both platforms' build steps, `.gitignore`, 17 new
  third-party licence texts in `LICENSES/` for a complete roster of
  19), pushed public to github.com/savannah-i-g/nanotracker (GPLv3;
  `include/ntp/` MIT). **CI** (`.github/workflows/ci.yml`): Linux
  (GCC) + Windows (MSVC/Ninja) jobs, ccache/sccache + `_deps` source
  caching, pinned Python for the glad generator; the Windows job
  uploads the beta artifact (exe + OpenAL Soft + libopenmpt DLL set +
  assets + licences). **Windows seam**, exactly the audited surface:
  `platform/shared_library.{h,cpp}` (dlopen/LoadLibrary behind one
  seam; clap_host and the lame loader route through it),
  `ext/editor_host_surface.{h,+x11.cpp,+win32.cpp}` (the plugin-editor
  host window abstraction — X11 impl carries the old code, Win32 impl
  is CreateWindowEx + message pump; Stage 20's VST3 editors inherit
  it), `platform/paths.cpp` (`GetModuleFileNameW`, `%APPDATA%`),
  CLAP Win32 search paths + `;` separator, posix-fd pump compiled out
  on Windows, CMake platform gating (X11, VST3 `_win32` TUs, MSVC
  flags, OpenAL Soft 1.25.2 FetchContent on WIN32, libopenmpt 0.8.3
  upstream VS2022 dev package pinned by SHA256). Fold-ins: S3M/IT
  importer binary fixtures + oracle tests (fixtures with teeth —
  mutation-checked; unsupported-effect warnings asserted
  counted-not-silent), the three stale promise comments rewritten as
  invariants, WinMM virtual-port refusal made explicit in midi_io
  (RtMidi only warns), dead theme helpers removed, snprintf buffers
  widened to silence GCC format-truncation analysis.
- Verification: both local trees green (41/41 release, 41/41
  ASan/UBSan); clang-tidy clean on every touched file; **CI green on
  both runners** — Linux 41/41, Windows 40 pass + 1 WARN-skip (MIDI
  loopback, honestly refused on WinMM); beta artifact downloaded and
  inspected (complete DLL/licence set); artifact boots under Wine,
  90 frames, exit 0 — recorded as smoke **proxy**, not validation
  (locked decision 4: GA promotion waits on community reports).
- Non-goal recorded: OpenAL buffer-queue fallback stays unbuilt
  (shipping OpenAL Soft dissolves it; revisit only on beta reports).
- Backup: `NanoTracker_stage13_2026-07-18.tar.gz`; git tag `stage-13`.

## 2026-07-17 — Stage 12 closed — Export + resilience; v1 feature-complete

- Landed: `io/ftrk_writer` — FTRK **version 14**: the full v13 layout
  (core sections, FXMX, INTB+BNDT, WPBR, PLGB, SEQB + channel
  colours, POVR passthrough, PPRS) plus the new XPLG block (external
  CLAP/VST3 state chunks with parameter snapshots for degraded
  restore) carried by a reserved header region grown 31→35 bytes; the
  reader accepts 1-14 and restores external plugins at load (reopen
  library → instantiate → load state, with a clear warning when the
  plugin is missing). `io/export_render` — offline render as a
  sample-clocked run of the real engine (fix #7): the project
  serialises to FTRK, loads into a fresh offline session, renders
  until the order list wraps + tail; WAV writes PCM16 directly, OGG
  encodes through libvorbis (FetchContent v1.3.5/v1.3.7), MP3 dlopens
  the system libmp3lame at run time (LGPL dynamic linking, clean
  refusal when absent). `app/autosave` — three rotating slots every
  120s under the config directory plus a session lock: an unclean
  exit leaves the lock and the shell offers "recover autosave" on the
  next launch. Shell window gained project save/load and export
  boxes; help and licence windows (keymap/workspace/plugin/sequence
  summaries; GPLv3 + full third-party list). `--save`/`--export` CLI
  hooks. `Docs/ftrk-format.md` published as the normative format
  spec. Session `save_ftrk` assembles everything live: WPBR from the
  workspace (dormant entries included), NTP archives referenced by
  nodes bundled into PLGB, PPRS from the preset bank, POVR passed
  through, XPLG from the live CLAP/VST3 instances.
- Verification: 37/37 tests normal and under ASan/UBSan; tidy CLEAN.
  New suites — the v14 round-trip test writes a project touching
  every block and reads it back field-for-field (packed bound slots,
  sample payloads, FX automation cells, BNDT, sequence notes, channel
  colours, WPBR/PPRS payloads, PLGB archives, XPLG state + f64
  params); the export test renders a generated project offline into
  all three formats, requires frame-identical lengths, and decodes
  the OGG/MP3 back through the app's own codecs with spectral checks
  (MP3 threshold looser for LAME's encoder-delay padding, noted
  inline). App-level: audible.ftrk exported to .wav/.ogg/.mp3 (5.80s
  each, 440/660 Hz content confirmed in the WAV); a project with
  sequence layers and workspace cables saved via --save and reloaded
  cleanly as v14; crash-journal semantics verified both ways (clean
  exit removes the lock, SIGKILL leaves it).
- v1 status: all twelve roadmap stages are closed. Punchlist update
  (2026-07-18): libasound2-dev installed, RtMidi rebuilt on the real
  ALSA backend — the MIDI loopback test round-trips note/CC events
  through a live virtual port and the PLL clock test measures 24 PPQN
  against a running 125 BPM transport; both suites are 37/37 with
  nothing skipped. Remaining: Windows validation needs a Windows
  build host or `mingw-w64` cross-toolchain (neither present). Also open from Stage 11's notes: pattern-record from MIDI
  input and live midi-kind cable transport through the graph runner
  (Ext MIDI In/Out pseudo-nodes) — infrastructure exists end to end,
  transport recorded as the first post-v1 work item.
- Backup: `NanoTracker_stage12_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 11 closed — MIDI + sequence layers

- Landed: `midi/midi_io` — RtMidi device management (hardware +
  virtual ports; input parses on RtMidi's callback thread into a
  lock-free ring the UI drains); `midi/midi_out_thread` — the
  dedicated MIDI output thread phase-locked against audio sample time
  (the engine snapshot now publishes total rendered frames + host
  time; the thread interpolates and dispatches at ~1ms), 24 PPQN
  clock from the transport with start/stop/song-position edges, plus
  a timestamped event queue; `midi/midi_learn` — arm-a-target learn
  state machine binding (channel, CC) to NTP/CLAP/VST3 parameters
  with range scaling, persisted as JSON in the config directory;
  `ui/midi_view` — device pickers, clock toggle with tick telemetry,
  live-instrument note entry (inbound notes audition sample or plugin
  instruments through the preview path), learn UI + mapping list;
  sequence layers made audible — the engine's golden-tested triggers
  (Stage 3) now drive a 32-voice layered pool for sample instruments
  and plugin bindings for the rest, mixed to master exactly like the
  web's per-layer instrument routing; `ui/piano_roll_view` — the
  sequence editor (pitch grid with black-key shading, row-snapped
  click-to-add, right-click delete, per-layer instrument/enable) and
  the two-octave on-screen keyboard with black/white styling and
  press/release audition; `--seq-demo` verification hook.
- Verification: 35/35 tests normal and under ASan/UBSan; tidy CLEAN.
  Sequence layers proven twice: a live-engine unit test (empty
  pattern, one long seq note → snapshot peak) and a sink capture of
  audible.ftrk + --seq-demo where the layer's pitch-84 notes put a
  1760 Hz line in the FFT at the same order of magnitude as the
  440/660 Hz pattern tones. Piano-roll screenshot with all eight demo
  notes, the keyboard and the MIDI window on record. The MIDI
  loopback suite (virtual-port round trip of note/CC events; PLL
  clock tick-rate check against a 125 BPM transport) is in place and
  currently skips with a recorded warning: RtMidi built its dummy
  backend because libasound2-dev is not installed — installing it and
  reconfiguring turns both tests live. Diagnosed via a temporary
  instrumented run (trigger count + scratch peak), removed after.
- Notes: pattern-record from MIDI input (live entry writes cells) and
  the graph's midi-kind cable transport (Ext MIDI In/Out pseudo-nodes
  routing through the runner) remain open — the cable kind, dormant
  carry and note-port infrastructure exist end to end, but live
  message routing through cables needs a midi-edge transport in the
  runner; recorded for the v1-close pass rather than half-landed
  here. Sequence-note edits are structural (stop + republish) because
  the scanner reads the note vectors concurrently.
- Backup: `NanoTracker_stage11_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 10 closed — External plugin hosting (CLAP + VST3)

- Landed: `ext/clap_host` — library loading (dlopen/clap_entry/
  factory), descriptor scan, instantiation with a real host surface
  (log, thread-check, posix-fd-support, timer-support), note-port
  dialect negotiation (plugins that prefer or only accept MIDI get
  3-byte events — u-he does), parameter enumeration + UI changes
  through a lock-free ring delivered as CLAP_EVENT_PARAM_VALUE, state
  save/load through memory streams, and RT process with planar
  staging; `ext/editor_window` — plugin GUIs as separate X11 OS
  windows (top-level + WM_DELETE protocol; the plugin embeds via the
  gui extension's set_parent), pumped once per UI frame with fd
  polling and due-timer dispatch so Linux editors don't starve;
  `ext/vst3_host` — VST3 on the SDK's hosting classes (Module/
  PlugProvider/HostProcessData/EventList/ParameterChanges; the SDK
  build system is bypassed — its hosting sources compile directly into
  a small static lib), bus arrangement + activation, normalized
  parameter surface, component+controller state chunks, note events;
  both hosts implement the same GraphPluginBinding NTP uses, so
  external plugins are workspace nodes with jack rails, the master
  strip, reroute suppression, instrument-table slots and the
  sequencer/preview note path with zero new engine code. Session
  catalogues + UI (load .clap/.vst3, add to workspace, auto-param
  panels — VST3 capped at the first 24 automatable params of
  synth-sized lists), `--load-clap/--clap-demo/--clap-editor/
  --load-vst3/--vst3-demo` verification hooks.
- Verification: 32/32 tests normal and under ASan/UBSan; tidy CLEAN
  (documented NOLINT blocks for CLAP/Xlib C-ABI array/union shapes;
  one analyzer path-warning inside the SDK's smartpointer.h accepted
  as third-party noise). New CLAP suite runs against a real dlopen'd
  .clap built from tests/fixtures/test_clap_plugin.c — descriptor
  scan, 440 Hz note rendering, param ring silencing the output, note
  off, state round-trip. Production-plugin verification with the
  installed u-he TyrellN6 in BOTH formats: CLAP capture RMS 0.045
  (held A-4 through load→instantiate→note→process→master→device) and
  VST3 capture RMS 0.045 — identical output from the same synth
  through both hosts; the CLAP editor opened as a real WM-managed
  1200×600 X11 window (verified read-only via the X window tree) and
  the app exited cleanly with it open.
- Notes: two use-after-scope bugs caught during bring-up — a
  sub-expression `classInfos()` temporary (bad_alloc via dangling
  ClassInfo) and the earlier nlohmann `.items()` pattern; both
  bound-then-used now. External-plugin state chunks persist to FTRK
  with the Stage 12 writer (save/load APIs are in place on both
  hosts). VST3 editor windows (IPlugView/IRunLoop) follow the CLAP
  editor shape post-v1; the auto-param panel covers VST3 UIs today.
- Backup: `NanoTracker_stage10_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 9 closed — NTP v1 plugin format

- Landed: `include/ntp/ntp_manifest.h` (MIT + plugin exception) — the
  single-version manifest schema as plain structs: params, typed
  ports, declarative UI controls, presets, capability requirements,
  and the DSP graph (16 node types incl. the reserved `native_stage`);
  `plugins/ntp_loader` — miniz archive read + strict collected-error
  validation (fix #10: load completely or refuse with reasons; webview
  UIs fall back to the auto-param panel with a note) + asset decode
  (samples device-rate, wavetables verbatim at source rate, images via
  stb_image); `plugins/ntp_graph` — node runtimes over the shared dsp
  primitives plus native ports of the granular (128-grain pool,
  jitter, scan, hann/triangle/rect envelopes, xorshift RNG — no
  library RNG on the audio thread) and wavetable (frame-morph
  oscillator) worklets, sampler zones (key/vel ranges, loop modes,
  round-robin, choke, release triggers, pitch tracking), oscillator/
  noise (white/pink/brown)/LFO (incl. sample-and-hold)/multi-stage
  envelope/constant/waveshaper/panner/compressor/biquad (notch added
  to dsp::Biquad)/delay/FIR convolver; `plugins/ntp_voices` — the
  instance: voice pool with stealing, per-voice vs shared node
  instantiation, cycle-break topology (same one-block-delay policy as
  the workspace), audio-rate `toParam` connections (envelope→gain,
  FM), k-rate mod routes with transform/scale/offset/slew, voiceIn/
  voiceOut/input/output pseudo-endpoints; `plugins/plugin_registry` +
  session integration — PLGB bundled archives join the catalogue at
  load, dormant WPBR plugin instruments wake with window placement +
  param snapshots, dormant cables retry once nodes exist, instrument-
  table plugin slots bind to instances through the bundle
  (`GraphPluginBinding` — the same interface external hosting will
  implement), sequencer note routing per channel with proper note-off;
  GraphRunner kPlugin processing with the volume/pan/bypass master
  strip and reroute suppression; `ui/ntp_ui` — declarative control
  rendering (rotary knobs, sliders, toggle/select/number, xy pad,
  envelope display, node-peak meters, labels, groups, image/sprite via
  GL textures) + factory/project/library preset picker + save-to-
  library; `plugins/preset_bank` — library JSON files in the config
  dir + PPRS project records (adopt + serialise); `tools/ntp-convert`
  — web manifest converter (legacy shorthand → graph synthesis,
  explicit unconvertible reports); `--load-plugin`/`--plugin-demo`
  verification hooks and a workspace-window plugin loader.
- Verification: 30/30 tests normal and under ASan/UBSan; tidy CLEAN.
  4 new NTP suites — strict validation collects all ten error classes
  from one bad manifest; a real in-memory ZIP loads and the synth
  voice puts a dominant 440 Hz line on the output via the pitch mod
  route and the envelope's audio-rate gain connection, then releases
  to silence and frees the voice; the FX graph echoes an impulse at
  exactly frame 480 for delayTime 0.01 s; sampler round-robin
  alternates 500/900 Hz zones with out-of-range keys silent. Live
  end-to-end: demo_pulsar.ntins (sawtooth/filter/envelope/LFO synth
  with knob/xy/envelope UI) loaded from disk, spawned as a workspace
  node, bound to instrument slot 1, held A-4 via the preview command —
  sink capture shows 440 Hz fundamental + 880 Hz sawtooth harmonic and
  nothing at non-harmonic 555 Hz; screenshot shows the PULSAR window
  (jack rail, preset combo, five knobs, xy pad, envelope curve,
  vol/pan/bypass strip). ntp-convert round trip: a legacy v2 web
  manifest (oscillators/envelope/filter shorthand + webview UI)
  converts with three explicit notes and the result loads cleanly in
  the native host.
- Notes: ASan caught a leak in the test ZIP helper
  (finalize_heap_archive transfers buffer ownership — now mz_free'd).
  A pre-C++23 nlohmann pitfall (`.value(...).items()` on a
  sub-expression temporary) bit twice and is now bound-then-iterated
  with comments. CV/gate plugin ports are visible but inert until
  their consumers land (cv→param with external hosting, midi with
  Stage 11). Sprite controls render statically; animation keys are a
  post-v1 driver feature.
- Backup: `NanoTracker_stage9_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 8 closed — Workspace + patch cables

- Landed: `graph/graph_model` (typed ports audio/sidechain/cv/gate/midi
  with the web's exact compatibility matrix, stable string port IDs,
  validated connect returning typed results, tap/reroute cables,
  built-in node builders — tracker bus, master in, module player,
  utility SUM); `graph/graph_compile` (flat schedule, per-jack reroute
  suppression, iterative-DFS cycle breaking: every back edge becomes a
  one-block feedback delay); `graph/graph_wpbr` (WPBR JSON adoption —
  portId-first, jackIndex fallback across the web's v5/v4/legacy port
  layouts with stored-kind arbitration; plugin-era instruments and MIDI
  cables carried verbatim as dormant entries; symmetric serialiser);
  `audio/graph_runner` (allocation-free block evaluation, double-
  buffered port pool, audio→cv downmix, audio→gate edge extraction
  with hysteresis at frame accuracy); engine restructure — voices
  render full-block channel scratch whether or not the transport runs
  (note preview now audible while stopped, matching the web), module
  player renders to its own scratch, graph evaluates once per block,
  dry mix honours reroute suppression, `kSwapBundle` live-swaps
  schedules so patching never stops playback; `ui/workspace_view`
  (floating no-dock node windows with jack rails, collapsed windows
  keep a compact jack strip, per-frame anchor registry, drop-target
  validity glow, layout write-back into the model) + `ui/cable_overlay`
  (verlet rope port of cablePhysics.ts, Catmull-Rom splines into the
  foreground draw list, kind colour/dash parity, midpoint tap/reroute
  chips, right-click delete, drag-to-connect with status-line
  feedback); cable physics settings in settings.json with web-parity
  clamps; input-script grammar gained mouse/mousedown/mouseup (with a
  backend fix: scripted positions survive the GLFW hardware-cursor
  fallback via the cursor-enter callback); `--workspace-demo`,
  `--workspace-dangle`, `--no-crt` verification hooks.
- Verification: 26/26 tests normal and under ASan/UBSan; tidy CLEAN.
  10 new graph tests — compatibility truth table, connect validation,
  cable rip on node removal, topological order + exactly-one delayed
  edge per cycle, self-loop delay, tap fan-out block math, CV fill,
  gate events at frames 40/90 exactly, WPBR era adoption and dormant
  round-trip. Feedback ring test: impulse recurs at unity in
  consecutive blocks through a sum1↔sum2 loop. Scripted UI run:
  mouse-drag from CH04 jack to MASTER IN committed a live cable
  (counter 4→5, status "ch04 -> main", screenshot). Sink-monitor
  captures of audible.ftrk: tap cable on ch01 raised 440 Hz magnitude
  0.00397→0.00535 with raw peak 0.164→0.287 (+6 dB pre-compressor
  duplication, squashed as expected); reroute-to-master preserved
  660 Hz (0.00545→0.00542); reroute into a dangling SUM killed it
  (0.00545→0.00006, 91×) — suppression and cable delivery proven in
  the live engine.
- Notes: FX rack now processes once per block (was per tick span), so
  FX automation is block-quantised (~2.7 ms at 48 kHz) — same order of
  magnitude as WebAudio param smoothing. ASan exposed a real hole in
  the debug allocator: the nothrow operator new/delete variants were
  not overridden, pairing the library's nothrow new with our free()
  (hit by stable_sort's temporary buffer) — all variants now route
  through the same malloc/free path and the RT check. Gate/CV inputs
  have no consumers until the plugin stages; the transports are built
  and tested at the runner level. MIDI cables stay dormant until
  Stage 11.
- Backup: `NanoTracker_stage8_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 7 closed — FX mixer + built-in DSP

- Landed: `audio/dsp.h` primitives (RBJ biquad, fractional delay line,
  bitcrusher at worklet parity, dB-domain compressor, Freeverb comb/
  allpass, DC blocker); `audio/fx_chain` — all eight web FX modules
  with their exact parameter schemas and topologies (DELAY, FILTER,
  BITCRUSHER, COMPRESSOR, DISTORTION incl. the sigmoid drive curve,
  CHORUS/FLGR, REVERB as documented Freeverb divergence, STEREO WIDTH
  M/S), assembled into FxRack strips with per-tracker-channel sends;
  engine mixer rework — per-channel scratch blocks feeding dry mix +
  sends, rack processing per block, tick-time FX automation
  application, always-on master compressor + DC blockers (web master
  chain tail); FX strips joined the engine project model and FTRK
  FXMX strips adopt on load (colors parsed); session FX APIs
  (structural rebuild vs live param commands, packed kFxParam);
  FX MIXER window (strips, sends, module stacks, registry picker).
- Verification: 140 deterministic DSP assertions (delay echo lands at
  the configured frame; 2-bit quantisation admits only 0.25 steps;
  width 0 collapses (1,-1) to mid-zero; rack respects send=0 isolation
  and live automation silences the chain); audible end-to-end — the
  bitcrusher at a 3 kHz hold rate on the 440 Hz fixture produces its
  aliasing images at exactly 2560/3440 Hz in the sink-monitor capture
  (667×/373× over the noise floor). Suite 16/16 normal + 16/16 ASan;
  tidy CLEAN.
- Backup: `NanoTracker_stage7_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 6 closed — Instruments + sample editors

- Landed: multi-format sample decode behind `audio/decoders`
  (dr_wav + dr_mp3 + stb_vorbis instantiated in one warnings-suppressed
  TU; format-sniffing `load_sample_memory`), so FTRK-embedded ogg/mp3
  samples now decode instead of warning; session slot management
  (`load_sample_into_slot`/`clear_slot` structural with bundle
  republish, `set_sample_meta` undoable with payload-field protection,
  `set_instrument_entry` with table growth + own-slot defaults);
  SAMPLES window (31-slot list, load/clear/preview, properties —
  volume/pan/base note/finetune/loops/category — min/max waveform
  display with loop-region overlay); INSTRUMENTS window (slot→sample
  mapping + bound-tracks matrix driving the pattern editor's
  instrument-free entry). Deterministic priming (fix #4): samples
  decode at load; a triggerable slot always has resident audio.
- Deferred, recorded: destructive waveform operations (trim/normalize/
  fade) and per-slot POVR overrides join the full instrument-editor
  work in the workspace stage; slot loads clear undo history (web
  parity) rather than snapshotting audio buffers.
- Verification: session tests (22 assertions — decode+resample 22050→
  48000, meta undo with payload protection, table growth + undo, clean
  failure paths); scripted screenshot shows both windows live with the
  real decoded waveform of the FTRK fixture's 440 Hz sample; suite
  12/12 normal + 12/12 ASan; tidy CLEAN (one real use-after-move
  caught and restructured).
- Backup: `NanoTracker_stage6_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 5 closed — Module playback + importers

- Landed: vendored libopenmpt 0.8.3 static build
  (`tools/build_libopenmpt.sh`, `third_party/`, excluded from backups;
  `Findlibopenmpt.cmake` resolves vendored-first then pkg-config);
  `modplay/module_player` (libopenmpt as an additive audio source:
  load/unload off-thread, RT render, position atomics, seek) +
  MODULE window (`ui/module_view`) + engine attach/play/stop commands;
  all four importers ported to `io/import/` — MOD (tag families,
  15-sample fallback, 8287→44100 upsample with loop scaling), XM
  (packed cells, keymaps, delta decode, volume column, version check,
  relNote sign pinned), S3M (parapointers, RLE rows, signed/unsigned
  PCM, stereo downmix), IT (channel-masked RLE with per-channel
  memory, IT 2.14/2.15 decompression at itsex.c parity, instrument
  keyboard tables); extension dispatch in ProjectSession::load_file;
  channel voices gained loop support (fix #9).
- Fixes ledger seeded: #1, #2, #7, #9, partial #12, plus a native-only
  IT decompressor corrupt-escape guard (latent negative-shift defect
  shared by the web original; caught by clang-analyzer).
- Verification: authored MOD fixture (looping square, period 214)
  plays at the Paula-exact 518 Hz through BOTH paths — libopenmpt node
  (4.4M× spectral dominance) and import→native sequencer (21k×) —
  captured off the sink monitor; import structure tests 25 assertions
  (MOD) + XM fixture with relNote +12 proving base_note 60 (sign pin)
  + rejection tests; total suite 11/11 normal + 11/11 ASan; tidy
  CLEAN. Known gap, recorded: S3M/IT importers lack binary fixtures
  (structural port verified by compiler/analyzer only) — real-module
  oracle tests against libopenmpt's pattern reads are planned when
  fixture modules join the tree.
- Backup: `NanoTracker_stage5_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 4 closed — Pattern editor + undo spine

- Landed: `app/undo` (closure-based command stack, bounded, redo branch
  invalidation) — the editing spine every later surface builds on;
  `app/project_session` (the deliberate Tracker.tsx decomposition:
  project + decoded samples + bundle publication + transport + undo-
  routed edits + note preview); `ui/pattern_view` (order list,
  transport strip, faithful grid port of trackerRenderer.ts — hue
  strips, bound-instrument pills, hex row numbers, beat-row elevation,
  playhead glow + follow-scroll, per-field cell colours; full
  TrackerCanvas keyboard model: note keys with octave, two-nibble hex
  entry, field navigation, Tab/PageUp/Down/Home/End, Delete-by-field,
  space/= note-off, F5/F8 transport, F1/F2 octave, Ctrl+Z/Y);
  `audio` preview-note command; in-memory WAV decode for
  project-embedded samples; exact web THEMES palette + 32-hue channel
  palette; `app/input_script` verification harness + `--load`,
  `--autoplay`, `--input-script` hooks.
- Incident: initial UI verification used xdotool against the live
  desktop and interfered with the user's session (focus stealing,
  keystrokes landing outside the app). Aborted on user report; replaced
  by the in-app input-script harness, now the doctrine-mandated UI
  automation path (13-verification.md).
- Verification: scripted end-to-end run — notes entered with
  auto-advance (C-5/D-5/E-5), volume hex entry (20), three-step undo
  restoring the exact prior state (screenshots), F5 playback with
  advancing row counter; audible FTRK fixture (web-serializer-built,
  real embedded WAVs) loads, decodes, and plays with both tones
  spectrally confirmed off the sink monitor. Build clean; tidy CLEAN;
  tests 8/8 normal + 8/8 ASan/UBSan.
- Backup: `NanoTracker_stage4_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 3 closed — Engine port + FTRK reader

- Landed: `engine/` — full port of the web sequencer state machine
  (data model `tracker_types.h`, `advance_tick` with the complete
  ProTracker effect set incl. deliberate web quirks: 5xx porta-speed
  reuse, stale volume-slide memory on 5xy/6xy, Bxx-over-Dxx precedence;
  sequence-layer scanning with fixed-capacity trigger array; RT-safe by
  construction). Sample-accurate transport inside the audio engine
  (tick boundaries in frames, block splitting at boundaries), per-
  channel pitched voices (period→rate, tremolo/slide volume follow,
  scaled 9xx offsets), PlaybackBundle RCU handoff, transport position
  in the UI snapshot. `io/ftrk_reader` — .ftrk versions 1-13
  (bounds-checked cursor; loud core failures; tolerant optional blocks;
  FXMX/INTB+BNDT/SEQB parsed into the model, WPBR/PLGB/POVR/PPRS
  carried in extras for their stages). `--play-demo` hook.
- Verification: **golden tick-traces** — 1,230 ticks across 10 fixtures
  dumped from the actual web engine (esbuild-bundled
  trackerEngine.ts under Node) reproduced exactly by the C++ engine
  (1,240 assertions, first run). **FTRK fixture** generated by the web
  serializer (all v13 blocks) loads byte-faithfully (530 assertions);
  corruption tests: bad magic/version/truncation fail loudly, damaged
  FXMX skipped with warning while the song loads. **Sequenced audio**
  capture-verified end to end: demo project kick (60 Hz, 620×
  dominance), lead C-4 (440 Hz, 15,134×), lead C-5 in its playback
  window (880 Hz, 80,195×). Tests 8/8 normal + 8/8 ASan/UBSan; tidy
  clean (cognitive-complexity check disabled with rationale — faithful
  ports of reference state machines are not split to satisfy metrics).
- Doc correction: `binarySerializer.ts` is MapManager code, not FTRK —
  citation fixed in `Plan_NativePort/10-formats-io.md`.
- Backup: `NanoTracker_stage3_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 2 closed — Audio core

- Landed: `rt/` primitives (SpscQueue with false-sharing padding,
  TripleBuffer latest-value handoff, RtScope + debug global-new/delete
  RT-violation aborts) with unit + two-thread stress tests;
  `audio/audio_device` pull contract + OpenAL Soft backend via
  `AL_SOFT_callback_buffer` (rate requested then queried, full-fill
  contract, extension checks); `audio/audio_engine` (fixed 128-frame
  interior block loop, UI→audio SpscQueue commands with drop counter,
  audio→UI TripleBuffer snapshot with block peaks, FTZ/DAZ denormal
  protection, test tone + one-shot sample voice); `audio/sample_buffer`
  (dr_wav decode → stereo normalise → libsamplerate sinc resample to
  device rate); shell audio panel (device status, tone, load-and-play,
  meters); `--tone`/`--play` automation hooks.
- Deliberately deferred (recorded, not silent): OpenAL buffer-queue
  fallback path until the Windows validation pass; loader-thread sample
  decode + sample retirement until the sampler stage (Stage 2 samples
  are process-lifetime).
- Verification: capture-based end-to-end audio proof — app output
  recorded off the PipeWire sink monitor and Goertzel-analysed: 440 Hz
  tone PASS (RMS 0.14, ~10.6M× dominance over off-frequencies); 880 Hz
  sample authored at 22050 Hz PASS (resample path produced exactly
  72001 frames at 48 kHz; ~1.4M× dominance). Tests 4/4 normal + 4/4
  ASan/UBSan; tidy clean; libsamplerate's own test suite forced out of
  ctest (BUILD_TESTING OFF; stale-build-tree pitfall noted: full
  reconfigure needed after the flag change).
- Backup: `NanoTracker_stage2_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 1 closed — Platform shell

- Landed: Dear ImGui v1.92.8-docking bootstrap (`platform/imgui_context`,
  docking enabled, io.IniFilename=nullptr, DPI-aware font rebuild);
  glad v2.0.8 GL loader owned by the platform layer
  (GLFW_INCLUDE_NONE propagated from the imgui target — GLFW's own
  build must not see it); Kode Mono bundled (4 static weights + OFL);
  theme system (`ui/theme`, amber/green/blue/red from web theme.css,
  derived ImGui style, sharp-cornered chrome); CRT pass (`ui/crt_pass`,
  scene FBO → bright-pass quarter-res glow blur → scanline/vignette
  composite, user-scalable); settings I/O (`io/settings` versioned JSON
  + `platform/paths` XDG config dir, tolerant load); shell UI with
  dockspace, theme switcher, CRT controls; `--frames N` flag for
  automated verification runs.
- Verification: build clean; clang-tidy CLEAN (misc-include-cleaner and
  easily-swappable-parameters disabled with documented rationale);
  ASan/UBSan tests pass; live X11 runs screenshotted at 0.35 and 1.00
  CRT intensity (amber theme, Kode Mono, glow/vignette confirmed);
  settings round-trip verified through real save→load cycles (window
  close path and --frames path).
- Backup: `NanoTracker_stage1_2026-07-17.tar.gz`.

## 2026-07-17 — Stage 0 closed — Scaffold

- Landed: repo layout per `Plan_NativePort/02-architecture.md` (src/app,
  src/platform, include/ntp, cmake, tests, tools, LICENSES); CMake with
  pinned FetchContent deps (GLFW 3.4, Catch2 v3.7.1);
  `Findlibopenmpt.cmake` (pkg-config route, package not yet installed —
  acquisition due Stage 5); LICENSE (GPLv3 full text) +
  `LICENSES/NTP-MIT.txt` (MIT + plugin exception); `.clang-format` +
  `.clang-tidy`; `tools/backup.sh`; `platform::AppWindow` (GLFW, GL 3.3
  core, vsync) and a minimal frame loop clearing to the amber theme
  background.
- Verification: build clean with `-Wall -Wextra -Wpedantic -Wconversion
  -Wshadow`; test suite passes normally and under ASan/UBSan
  (`build-asan`); app launched on the X11 session and rendered until a
  3s timeout killed it (exit 124).
- Gap: clang-format/clang-tidy binaries are not installed on the dev
  machine — configs are in-tree but the format/tidy gate could not run.
  Install before Stage 1 closes.
- Backup: `NanoTracker_stage0_2026-07-17.tar.gz`.

## 2026-07-17 — Planning complete

- Full design doc tree landed at `Docs/Plan_NativePort/` (00-14).
- Decisions locked: CLAP→VST3 both in v1, GPLv3 + MIT ntp headers;
  hybrid window feel; Linux-first portable-clean.
- Web app explored end to end (three parallel surveys + design
  stress-test); 16 fix-don't-retain items catalogued in
  `Plan_NativePort/01-context.md`.
- Research burst complete: 6 topic files under `Plan_NativePort/Research/`
  with citations; headline finding — VST3 SDK went MIT (Nov 2025), GPLv3
  reaffirmed by the owner on its merits; libopenmpt no-extraction and
  callback-buffer constraints verified; all findings folded into the
  design docs (applied log in `Research/00-index.md`).
- Verification: n/a (no code yet).
- Backup: `NanoTracker_planning_2026-07-17.tar.gz`.
