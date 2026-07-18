# 11 — Post-v1 Roadmap

Stages ordered by dependency and risk (no timescales, per doctrine).
Numbering continues from v1. Each stage closes via the v1 exit
checklist (`../Plan_NativePort/12-doctrine.md`) with one amendment:
backups become **git tag + tar**.

| Stage | Delivers | Key deps | Risk center |
| --- | --- | --- | --- |
| 13 | Repo + publish scrub + CI both platforms + Windows seam port + BETA artifact; S3M/IT fixtures; stale comments fixed | — | Windows CI toolchain + OpenAL Soft/libopenmpt acquisition |
| 14 | Pattern selection/copy/paste/transpose/interpolate; piano-roll multiselect + toolbox; INSTRUMENTS source picker; ballistic meter | 13 (CI green) | keyboard-model feel |
| 15 | Export suite: range, stems+ZIP, bit depths, quality knobs, fades + peak/true-peak/LUFS, metadata, presets, export window | 13 | LUFS correctness |
| 16 | Generation-fenced sample reclamation; destructive waveform ops + undo; sample browser | 13 | reclamation vs audio callback |
| 17 | Runner midi transport + Ext MIDI In/Out; pattern record + step entry; effect→MIDI; CV→plugin params | 13 | RT event transport |
| 18 | Local API: WebSocket server + schema (fix-list #5 paid) + window | 13 | thread marshalling |
| 19 | POVR user slots live + slot picker; sliceMap chopping | 16 (buffer lifecycle) | override lifetime |
| 20 | Partitioned-FFT engine (convolver uncap + REVERB parity); sprite animation; envelope editing; VST3 editors both OSes | 13 (editor seam), 17 settled | FFT RT scheduling |
| 21 | native_stage C-ABI v1 + fixture; GA packaging both platforms; release | 17, 20 settled | ABI freeze |

Cycle completion = all stages closed + Windows GA promotion criteria
met (community confirmation) + site features page pointing at
downloads.

Parked (recorded, not promised): out-of-process plugin bridging,
multi-viewport patchbay, additional AudioDevice backends, project
browser / asset manager windows, history panel, help-manual depth
(site), transient slice auto-detection (grid fallback ports),
OpenAL buffer-queue fallback (revisit only on beta callback reports).
