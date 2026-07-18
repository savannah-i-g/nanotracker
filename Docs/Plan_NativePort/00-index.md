# NanoTracker Native Port — Design Documents

Read order for a newcomer: 01 (why) → 02 (architecture spine) → 03-11
(subsystems, any order after 02) → 12 (doctrine) → 13 (verification) →
14 (roadmap). `Research/` holds the web-research trail with citations.

## At a glance

- Port of the web NanoTracker (React/TS/WebAudio, ~42K LOC at
  `Source/federated-industries-main/src`, route `/tracker`) to native
  C++20 on OpenAL Soft + OpenGL + Dear ImGui (docking branch).
- Own float32 DSP graph on a real-time audio thread; OpenAL is only the
  output device (pull-model callback via `AL_SOFT_callback_buffer`).
- libopenmpt provides faithful MOD/XM/S3M/IT playback as a graph node;
  the four hand-written importers are ported for module→project editing.
- Native plugin format "NTP v1": declarative ZIP+JSON, single schema
  version, typed ports, ImGui-rendered UI.
- External plugins: CLAP hosting first, VST3 within v1 scope after it.
- Sequencer runs on the audio thread, sample-accurate; the web's
  lookahead scheduler is not ported.

## Decisions locked (2026-07-17)

1. CLAP first, then VST3, both in v1; project license GPLv3 from day one;
   `include/ntp/` headers MIT with plugin exception. (GPLv3 reaffirmed
   2026-07-17 after research showed VST3 went MIT in Nov 2025 — see
   Research/04 — so copyleft is now chosen on its merits, not forced.)
2. Window feel: hybrid — instrument/patchbay windows always free-floating,
   editor surfaces optionally dockable.
3. Platform: Linux-first, strict platform layer, Windows validated at
   major stage boundaries.

## Contents

| Doc | Concern |
| --- | --- |
| [01-context.md](01-context.md) | Why the port exists; source app anatomy |
| [02-architecture.md](02-architecture.md) | Module decomposition, threading model, repo layout |
| [03-audio-backend.md](03-audio-backend.md) | OpenAL device layer, block model, sample rates |
| [04-engine.md](04-engine.md) | Tracker sequencer port, golden-vector testing |
| [05-module-playback.md](05-module-playback.md) | libopenmpt node, importer ports |
| [06-graph-cables.md](06-graph-cables.md) | Patch-cable graph, typed ports, feedback, rendering |
| [07-plugins-ntp.md](07-plugins-ntp.md) | Native plugin format (NTP v1) |
| [08-external-plugins.md](08-external-plugins.md) | CLAP + VST3 hosting, licensing |
| [09-windows-ui.md](09-windows-ui.md) | ImGui windowing, visual identity, CRT shader |
| [10-formats-io.md](10-formats-io.md) | FTRK v13/v14, settings, autosave, export |
| [11-midi.md](11-midi.md) | MIDI I/O, clock, learn |
| [12-doctrine.md](12-doctrine.md) | Codebase standards, ledgers, backups |
| [13-verification.md](13-verification.md) | Test strategy per subsystem |
| [14-roadmap.md](14-roadmap.md) | Stages by dependency + risk |
| Research/ | Web-research findings (created in the research phase) |

## Fix-don't-retain ledger seed

Sixteen web behaviours identified as broken/patchy during exploration are
enumerated in [01-context.md](01-context.md#fix-dont-retain-list); each
becomes a `Docs/FIXES.md` entry when its fix lands.
