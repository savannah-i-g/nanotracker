# 10 — Formats, Persistence, Export

## FTRK: keep and extend

The web project format is already the right design: a versioned,
little-endian, typed-block binary container (magic "FTRK", version 13 —
`Source/federated-industries-main/src/lib/trackerSerializer.ts:77-86`,
which is the format's sole normative source; `binarySerializer.ts`
belongs to MapManager, a mis-citation corrected during Stage 3).
Blocks: FXMX
(FX mixer), INTB (+BNDT trailer, instrument table), WPBR (workspace
patchbay + window snapshots), PLGB (bundled plugins), SEQB (sequence
layers), POVR (sample overrides), PPRS (plugin presets). Samples are
stored as their original encoded bytes (wav/ogg/mp3) — lossless
round-trip, small files. A new container would buy nothing and cost
migration fidelity.

Native behaviour:

- **Reads v13** — web projects migrate in losslessly.
- **Writes v14**, adding: external-plugin state block (CLAP/VST3 IDs +
  state chunks + param snapshots), native plugin registry IDs, editor
  layout block (docked-surface layouts; instrument window snapshots stay
  in WPBR as before), and a **required-features bitmask** so older
  readers fail loudly instead of half-loading.
- `Docs/ftrk-format.md` becomes the normative spec, written from the two
  web serializer files during the FTRK stage. The web app never needs to
  read native saves (the native app becomes the default).
- Compatibility note: sample loop points keep source-rate semantics in
  the file; the native sampler converts to play-buffer frames at load
  ([04-engine.md](04-engine.md#sampler-runtime)).

## Settings

Web settings live in per-module localStorage JSON blobs
(`trackerDisplaySettings.ts`, `trackerAudioSettings.ts`,
`trackerGeneralSettings.ts`, `trackerVisualSettings.ts`,
`trackerSfx.ts`, `midiMappings.ts`, window-layout/cable settings).
Native: one versioned JSON settings file per concern under the platform
config dir (`platform/paths`), one clean schema — the web's
never-existed settings that fell through to hardcoded defaults
(`TrackerSettingsWindow.tsx:97`, fix #6) are not carried over.

<a name="autosave"></a>
## Autosave and crash recovery

Web: IndexedDB circular slots + localStorage descriptor
(`trackerAutosave.ts:30,129`, `projectStore.ts:59-78`). Native:

- Circular autosave slots as FTRK files in the config dir, written by
  the loader pool off the UI thread.
- **Crash-recovery journal** (v1 scope, locked decision): dirty-project
  journal flushed periodically; on launch after a crash the app offers
  recovery. In-process external plugins make this insurance mandatory
  ([08-external-plugins.md](08-external-plugins.md#crash-posture)).
- Undo/redo: explicit command architecture landing with the first
  editing surface (Stage 4) — every subsequent editor builds on it. The
  web leaned on React state snapshots (`trackerUndoManager.ts`);
  retrofitting commands later is the known-miserable path.

<a name="export"></a>
## Export

Web: offline render via OfflineAudioContext + wasm encoders
(`exportRenderOffline.ts`, `src/lib/exportFormats/`), capture via
deprecated ScriptProcessorNode (`trackerAudio.ts:1055-1088`, fix #7).

Native: offline export runs the *same* graph runner clocked by sample
count instead of the device callback — export cannot drift from
playback by construction. Encoders: WAV (dr_wav write path is trivial),
OGG (libvorbis), MP3 (LAME — patents expired, LGPL dynamically linked).
Export UI ports `TrackerExportWindow.tsx` semantics (range, format,
sample rate choices).

## Project folders

Web's File System Access API project folders (`projectFs.ts`) become
plain native directory handling: a project folder holds the `.ftrk`
plus loose plugin archives, exactly as the web's "SAVE TO PROJECT"
copies plugins alongside (`TrackerPluginsPanel.tsx` flow).

## File map

- `src/io/ftrk_reader.{h,cpp}`, `ftrk_writer.{h,cpp}`,
  `binary_io.{h,cpp}` — container + blocks
- `src/io/settings.{h,cpp}` — versioned JSON settings
- `src/io/autosave.{h,cpp}` — slots + crash journal
- `src/app/undo.{h,cpp}` — command spine
- `src/io/export_render.{h,cpp}` + `src/io/encoders/*.cpp`
- `Docs/ftrk-format.md` — normative spec (written at Stage 3)
