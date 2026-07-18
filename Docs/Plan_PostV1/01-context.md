# 01 — Context: What v1 Left Open

Three exploration sweeps (2026-07-18) ground this cycle. This doc is
their condensed record; stage docs cite it rather than repeating it.

## A. Deferral inventory (from ledgers + code comments)

Seventeen recorded items. The load-bearing ones:

- **midi-kind cable transport** — the runner drops kMidi edges
  (`src/audio/graph_runner.cpp:54`, `src/graph/graph_compile.cpp:82`);
  everything else (cable kind, dormant carry, note-port dialects) is
  live. PROGRESS names this the first post-v1 work item. → Stage 17.
- **Stale promises** (comments referencing closed stages):
  loader-pool sample retirement (`src/audio/audio_engine.h:74`,
  `src/app/main.cpp:154`, `src/app/project_session.h:246` — samples
  are never freed until shutdown); ballistic shell meter
  (`src/app/main.cpp:290`); INSTRUMENTS-window plugin/workspace
  source picker (`src/ui/instrument_table_view.h:4` — the backend
  binding exists end to end, only the picker is missing).
  → Stages 16 (retirement), 14 (picker + meter); comments fixed in 13.
- **One shared dependency**: a partitioned-FFT convolution engine
  unlocks BOTH the NTP convolver 2048-tap cap
  (`src/plugins/ntp_graph.h:15`) and REVERB convolution parity
  (`Docs/FIXES.md` reverb entry). → Stage 20, one engine, two payoffs.
- **CV→plugin-params** — the runner routes CV blocks but kPlugin
  nodes never read their CV inputs (`src/audio/graph_runner.cpp:257`);
  instance param setters exist on all three plugin kinds. → Stage 17.
- Also open: VST3 editor windows (CLAP editor is the model),
  sprite animation, envelope editing, `native_stage` C-ABI
  (`include/ntp/ntp_manifest.h:49`), S3M/IT binary fixtures,
  pattern-record from MIDI.

## B. Parity audit (what a web user would miss), ranked

1. **Export options** — the web has order-range, stems (channel
   mask), WAV 16/24/32f, MP3 bitrate / OGG quality, fade +
   peak/true-peak/LUFS normalize (BS.1770), metadata, presets, ZIP
   (`lib/exportRenderOffline.ts`, `lib/exportPostProcess.ts`,
   `components/TrackerExportModal.tsx`). Native: fixed-quality
   wav16/ogg/mp3 + tail. The biggest single narrowing.
2. **Waveform destructive editing** — `components/WaveformEditor.tsx`
   (1023 LOC): trim/silence/fades/normalize/reverse/gain/DC with undo
   and zoom. Native `ui/sample_view.cpp`: loop points + metadata only.
3. **Pattern + piano-roll conveniences** — native lacks even
   single-cell copy/paste; the web piano roll has a full transform
   toolbox (`SequencePianoRollEditor.tsx:788-799`) + multi-select.
4. **Sampler platform** — POVR user-slot overrides round-trip but the
   feature is dead natively (a shared `.ftrk` plays the baked sample,
   not the user's); sliceMap (MPC chopping) is unimplemented.
5. **Plugin UI richness** — webview plugins fall back to the param
   panel (by design), sprites render statically, the envelope editor
   is display-only.
6. **MIDI** — `lib/trackerEffectToMidi.ts` (row effects → CC /
   pitch-bend / BPM) is unported; no MIDI record or step-entry.
7. **Local API** — the WebSocket remote-control surface
   (`lib/trackerLocalApi.ts`, ~31KB) is absent. Savannah uses it →
   ported this cycle (Stage 18).
8. Convenience tier, deliberately offloaded to the site: help manual
   depth (`TrackerHelp.tsx`, 3042 LOC), history panel, project
   browser, asset manager.

## C. Release-engineering ground truth

- **Zero platform guards in src/** — the Windows surface is exactly
  four source files + CMake gating (see `02-release-engineering.md`).
- **Dependencies**: every FetchContent pin builds on Windows. Three
  exceptions: OpenAL Soft must be *shipped* (the stock Windows
  OpenAL32.dll lacks `AL_SOFT_callback_buffer`, which the device
  hard-requires); libopenmpt's vendor script is bash-only (Windows →
  upstream prebuilt or vcpkg); MP3 needs a documented optional DLL.
- **Publish scrub**: `build*/` and `third_party/` are generated
  (backup.sh already excludes them); MISSING for a public GPLv3
  release: README, `.gitignore`, CI config, and ~14 of ~16
  third-party license texts in `LICENSES/`.
