# 14 — Roadmap

Stages ordered by dependency and risk. No timescales — progress is
logged per stage in `Docs/PROGRESS.md`; each stage closes via the exit
checklist in [12-doctrine.md](12-doctrine.md#stage-exit-checklist).
Undo/redo is not a stage: the command architecture lands with Stage 4
and every later editing surface builds on it.

| Stage | Delivers | Key deps | Risk center |
| --- | --- | --- | --- |
| 0 | Scaffold: repo layout, CMake + pinned deps, licenses, clang-format/tidy, backup script, GL window opens | — | libopenmpt acquisition |
| 1 | Platform shell: ImGui docking bootstrap, Kode Mono, 4 phosphor themes, CRT pass, settings I/O | 0 | DPI/scale handling |
| 2 | Audio core: AudioDevice (callback-buffer), audio thread, block loop, command queue + snapshots, WAV decode, first voice audible | 0 | RT plumbing correctness |
| 3 | Engine port: advance_tick + golden vectors, sample-accurate transport, FTRK v13 reader | 2 | effect-matrix fidelity |
| 4 | Pattern editor: grid + order list, cell editing, keymap, volume mixer, undo spine — core tracker loop playable | 1,3 | editing feel/latency |
| 5 | Module playback: libopenmpt node + import (4 parsers, fixed), import report UI | 2,3 | importer fixes |
| 6 | Instruments + samples: instrument table, sampler runtime (deterministic priming), instrument + waveform editors | 4 | InstrumentWindow scope (76KB web source) |
| 7 | FX mixer + built-in DSP: FX chains, bitcrusher/granular/wavetable ports, master retro chain | 2 | DSP parity with worklets |
| 8 | Workspace + cables: graph model, compile/schedule, cable UI + verlet, jack rails, tap/reroute, block-accurate gate/CV, feedback delay | 6,7 | graph compiler |
| 9 | NTP v1: manifest schema + validation, DSP interpreter, declarative ImGui UI, presets, PLGB bundling, web→native converter | 8 | schema finalisation |
| 10 | External hosting: CLAP first, then VST3; ext nodes, state chunks, editor OS windows, auto-param panel | 8 | VST3 run-loop/X11 |
| 11 | MIDI + sequence layers: RtMidi I/O, learn, PLL clock out; piano roll + on-screen keyboard | 4,8 | clock jitter |
| 12 | Export + resilience: offline render WAV/OGG/MP3, autosave slots + crash journal, help/licence windows | 5,7 | export determinism |

v1 completion = all stages closed + Windows build validated
([00-index.md](00-index.md) decision 3) + `Docs/ftrk-format.md`
published. Post-v1 backlog (recorded, not promised into stages):
multi-viewport patchbay, NTP `native_stage` C-ABI escape hatch,
out-of-process plugin bridging, additional AudioDevice backends.
