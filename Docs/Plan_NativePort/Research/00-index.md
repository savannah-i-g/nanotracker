# Research Index

Web-research burst run 2026-07-17, after plan approval. Each topic file
carries findings, plan implications, and sources.

## Would-change-the-plan items

1. **VST3 SDK is now MIT-licensed** (Steinberg relicensed in November
   2025; confirmed on Steinberg's own developer portal). The locked
   license decision ("GPLv3 from day one") was premised on VST3 forcing
   GPLv3 — that premise is gone. The project license is again a free
   choice; CLAP-before-VST3 ordering still stands on simplicity grounds
   alone. See [04-clap-vst3-licensing.md](04-clap-vst3-licensing.md).
   → Decision re-put to the project owner; outcome recorded in
   [00-index.md](../00-index.md) and 08.
2. **CLAP editor windows are host-created, plugin-embedded** (XEmbed on
   X11; host must implement the `posix_fd_support` and `timer_support`
   extensions; embedding does not exist on Wayland). Refines the editor-
   window design in [08-external-plugins.md](../08-external-plugins.md)
   — the host owns a top-level X11 window per editor and the plugin
   embeds into it. See [05-clap-hosting.md](05-clap-hosting.md).
3. **Dear ImGui ships dedicated docking release tags**
   (`v1.92.x-docking`; 1.92.8 WIP active April 2026). Pin the newest
   docking tag at Stage 0 rather than tracking the branch head. See
   [01-imgui-docking.md](01-imgui-docking.md).

## Confirmations (plan stands)

- **libopenmpt cannot extract sample/instrument data** — its FAQ calls
  it a pure playback library; third parties fork it to get extraction.
  Confirms porting the four hand-written importers
  ([03-libopenmpt.md](03-libopenmpt.md)). Current stable 0.8 (May 2025).
- **AL_SOFT_callback_buffer** works as designed: mixer-thread pull, RT-
  safe callback required, callback buffer must sit on a single static
  source (not queued) — fits the one-streaming-source design
  ([02-openal-callback.md](02-openal-callback.md)).
- **ALC_FREQUENCY** is a request, not a guarantee; query the actual rate
  after context creation — matches the "run graph at device rate,
  query actual" design ([02-openal-callback.md](02-openal-callback.md)).
- **Wayland fractional scaling is a live upstream problem** for
  GLFW+ImGui (blurry rendering, imgui#7433) — validates the dedicated
  DPI task and the X11-fallback posture
  ([06-platform-dpi.md](06-platform-dpi.md)).
- **Prior art for ImGui node/cable UIs exists** (imgui-node-editor,
  imnodes) — useful for bezier hit-testing technique, though our cable
  rendering stays custom verlet
  ([06-platform-dpi.md](06-platform-dpi.md#node-editor-prior-art)).

## Applied-findings log

Maintained as findings are folded into the design docs; each line names
the finding, the file edited, and the change.

- [x] (1) VST3 MIT — `../08-external-plugins.md`, `../00-index.md`,
      `../02-architecture.md` license rows/decision text updated after
      owner decision.
- [x] (2) CLAP embedding — `../08-external-plugins.md` editor-window
      section rewritten (host-created X11 window + XEmbed + fd/timer
      extensions).
- [x] (3) Docking tag pin — `../02-architecture.md` dependency table
      notes `v1.92.x-docking` tag pinning.
- [x] libopenmpt confirmation — `../05-module-playback.md` marked
      verified with FAQ citation.
- [x] callback-buffer static-source constraint —
      `../03-audio-backend.md` pull-model section.
- [x] Wayland DPI — `../09-windows-ui.md` DPI note strengthened.
