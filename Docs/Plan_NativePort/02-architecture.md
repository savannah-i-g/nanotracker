# 02 — Architecture Spine

## Module decomposition

The web app's worst structural habit is `src/pages/Tracker.tsx` (3,599
LOC) owning project state, transport, audio engine wiring, and every
window. The native decomposition is designed against that: one concern
per module, narrow headers, and a hard UI/audio boundary.

```
Output/
├── CMakeLists.txt
├── LICENSE               (GPLv3)
├── LICENSES/             (per-dependency license texts + NTP MIT text)
├── include/ntp/          (public plugin ABI headers — C, MIT + plugin exception)
├── src/
│   ├── app/              (main, app lifecycle, project session, undo spine)
│   ├── platform/         (GLFW window/GL context, ImGui bootstrap, DPI, paths)
│   ├── rt/               (SPSC queue, triple buffer, RT-safety asserts)
│   ├── audio/            (AudioDevice, audio thread, graph runner, mixer strips)
│   ├── engine/           (tracker sequencer — pure, I/O-free)
│   ├── modplay/          (libopenmpt Module Player node)
│   ├── graph/            (patch-cable model: nodes, typed ports, cables)
│   ├── plugins/          (NTP loader, declarative DSP interpreter, presets)
│   ├── ext/              (CLAP host; VST3 host when it lands)
│   ├── midi/             (RtMidi I/O thread, clock out, MIDI-learn)
│   ├── ui/               (ImGui views: pattern, instrument, sample, mixers,
│   │                      cables overlay, window registry, themes, CRT pass)
│   └── io/               (FTRK read/write, importers, settings, autosave, export)
├── tests/                (Catch2; golden vectors, format round-trips)
├── tools/                (web→native plugin manifest converter, trace dumper)
├── assets/               (Kode Mono + OFL text, icons, factory content)
└── Docs/                 (this tree, PROGRESS.md, FIXES.md, DEPENDENCIES.md)
```

Dependency direction (arrows = "may include headers of"):

```
ui  → app → engine, graph, io
ui  → rt (snapshots)          audio → rt, engine, graph, modplay, plugins, ext
io  → engine, graph, plugins  midi → rt
platform ← everyone (init only; no upward knowledge)
engine → (nothing but std)    rt → (nothing but std/atomic)
```

`engine/` stays pure and I/O-free exactly like the web original
(`Source/federated-industries-main/src/lib/trackerEngine.ts:1-5` states
the contract; the port keeps it — see [04-engine.md](04-engine.md)).

## Threading model

Four threads plus a pool:

- **UI/render thread** — GLFW events, ImGui, GL, cable verlet sim. Never
  touches audio state directly.
- **Audio thread** — pulled by the OpenAL Soft mixer via
  `AL_SOFT_callback_buffer` ([03-audio-backend.md](03-audio-backend.md)).
  Runs the sequencer tick state machine sample-accurately (row/tick
  boundaries computed in frames) and evaluates the DSP graph in fixed
  blocks. The web's lookahead scheduler
  (`src/lib/transportClock.ts:139-311`) existed because the sequencer
  could not live in the audio callback; ours can, so the scheduler is
  deliberately not ported.
- **MIDI thread** — RtMidi callbacks in, clock/output events out, PLL'd
  against audio sample time ([11-midi.md](11-midi.md)).
- **Loader pool** — sample decode/resample, plugin archive loading,
  autosave writes.

Communication:

- UI → audio: lock-free SPSC command queue (`rt/`). Commands are POD;
  graph mutations are RCU-style pointer swaps built off-thread, retired
  memory freed off-thread.
- Audio → UI: per-block triple-buffered snapshot (playhead position,
  channel meters, gate/CV visualisation values). The UI never reads
  engine state directly; this path is designed in from Stage 2, not
  bolted on.
- RT discipline: no allocation, locks, or syscalls on the audio thread;
  a debug-mode allocation assert guards this from day one. FTZ/DAZ set on
  audio and offline-render threads (denormal tails otherwise crawl in
  convolver/feedback paths).

## Build and dependencies

C++20, CMake. All dependencies via FetchContent with pinned tags, except
libopenmpt (no upstream CMake): system pkg-config with a vendored
fallback behind `cmake/Findlibopenmpt.cmake`.

| Dependency | Role | License |
| --- | --- | --- |
| GLFW | window/context/input | zlib |
| glad | GL loader | MIT/Public |
| Dear ImGui (docking branch) | UI | MIT |
| OpenAL Soft | audio device | LGPL (dyn-linked) |
| libopenmpt | module playback | BSD-3 |
| RtMidi | MIDI I/O | MIT-like |
| dr_wav / dr_mp3 / stb_vorbis | sample decode | public domain/MIT |
| libogg + libvorbis | OGG export | BSD |
| LAME | MP3 export | LGPL (dyn-linked) |
| libsamplerate | load-time resampling | BSD-2 |
| miniz | plugin ZIP archives | MIT |
| nlohmann-json | manifests/settings | MIT |
| Catch2 v3 | tests | BSL-1.0 |
| CLAP headers | external plugin hosting | MIT |
| VST3 SDK (later in v1) | external plugin hosting | MIT (relicensed Nov 2025 — Research/04) |

Dear ImGui is pinned to the newest `v1.92.x-docking` release tag (the
docking branch ships dedicated tags — Research/01), bumped deliberately
and recorded in `Docs/DEPENDENCIES.md`.

License structure is a locked decision: program GPLv3 from day one,
`include/ntp/` MIT with an explicit plugin exception
([08-external-plugins.md](08-external-plugins.md)).
`Docs/DEPENDENCIES.md` tracks exact pinned versions.

## Platform policy

Linux-first ([00-index.md](00-index.md) decision 3). `platform/` is the
only module allowed OS-conditional code. GLFW chooses its native backend;
external plugin editors (X11) run under XWayland when the session is
Wayland. Windows build validated at major stage boundaries; nothing in
app code may assume paths, casing, or clocks beyond the platform layer.

## File map (representative first landings)

- `src/rt/spsc_queue.h`, `src/rt/triple_buffer.h`, `src/rt/rt_assert.h`
- `src/audio/audio_device.h` (+ `audio_device_openal.cpp`)
- `src/audio/graph_runner.{h,cpp}`
- `src/engine/tracker_engine.{h,cpp}`, `src/engine/tracker_types.h`
- `src/app/main.cpp`, `src/platform/app_window.{h,cpp}`,
  `src/platform/imgui_context.{h,cpp}`
