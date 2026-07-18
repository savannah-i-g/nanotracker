# 07 — Native Plugin Format (NTP v1)

## Design position

The web plugin system's best idea is kept: **plugins are data, not
code**. A plugin is a ZIP archive (`.ntins` instrument / `.ntsfx` FX)
containing `plugin.json` plus assets (samples, impulses, sprites,
fonts). All DSP is a declarative node graph the host interprets; all UI
is declarative controls the host renders. That is what makes public
plugin sharing safe — nothing in an archive executes.

What is *not* kept: five schema versions of migration debt
(`Source/federated-industries-main/src/lib/pluginRegistry.ts:82`,
`migrationV3toV4.ts`), the dual positional/typed port model
(`pluginInsAdapter.ts:94-113`), webview iframes, and the AudioWorklet JS
escape hatch. NTP v1 is a single, clean schema version — the web format
stays as-is on the web side, and a converter bridges them.

## Manifest

`plugin.json` concepts map 1:1 from the web types
(`src/lib/pluginTypes.ts`):

- Identity: name, version, type (`instrument | fx | control-source`),
  NTP schema version (exactly one valid value in v1).
- Params (`PluginParamDef` web `:27-48`): key, label, range, default,
  step, curve, midi-learnable.
- DSP graph: nodes from the built-in set — gain, delay, biquad,
  compressor, convolver, panner, waveshaper, mixer, splitter/merger,
  oscillator, constant, lfo, envelope, granular, wavetable, sampler
  (web union `pluginTypes.ts:90-115`) — plus connections and mod routes.
  The web's `worklet` node type is replaced by a **reserved**
  `native_stage` node type: parsed and rejected with a clear message in
  v1, defined for a post-v1 C-ABI escape hatch so manifests are forward
  compatible. The ABI is deliberately not frozen while the graph engine
  is still settling; that is how the web format accrued its v1-v5 debt.
- Ports: typed, stable string IDs, kinds as in
  [06-graph-cables.md](06-graph-cables.md). Implicit midi-in/midi-thru
  for instruments with opt-outs (web `pluginTypes.ts:949-957`).
- UI: declarative controls — knob, slider, xy_pad, envelope_editor,
  sprite, meter, label — rendered natively in ImGui with the plugin's
  declared layout. Sprite assets load from the archive (the web's
  Firebase fetch dies; the control type does not). No webview: an
  archive that only declares a webview UI gets the **auto-param panel**
  (generated sliders from the param list — shared with external plugin
  hosting, [08-external-plugins.md](08-external-plugins.md)).
- Presets: factory presets in-manifest; user presets in the preset bank
  ([10-formats-io.md](10-formats-io.md)) and per-project FTRK blocks.
- Capabilities: `requires[]` checked at load; unknown requirement =
  clean refusal with message (strict load-time validation replaces the
  web's "degraded" fallback, `pluginInstrumentGraph.ts:562`, fix #10).

## Interpreter

`src/plugins/` materialises the manifest graph into native graph nodes
(same node implementations the FX mixer and master chain use). Voice
handling for instruments ports the web's per-voice graph concept
(`pluginTypes.ts:597-644`): voice pool, per-voice envelopes/oscillators,
shared FX tail. Granular/wavetable/sampler nodes are the native ports of
the corresponding worklets (`public/audioworklets/granular.js`,
`wavetable.js` — parameter sets preserved).

## Bundling and conversion

- Projects can embed plugin archives (FTRK PLGB block) exactly as the
  web does (`pluginRegistry.ts:36,99`), so songs stay self-contained.
- `tools/ntp-convert/` — web→native manifest converter (JSON→JSON;
  concepts map 1:1). It reports anything unconvertible (worklet nodes,
  webview-only UIs) explicitly. The web plugin catalogue survives the
  transition instead of being orphaned.
- Native discovery: a plugins directory scanned at startup + manual
  load; the registry is a plain code module, not a singleton grab-bag.

## File map

- `include/ntp/ntp_manifest.h` — the manifest schema as C structs (MIT;
  the future C-ABI home)
- `src/plugins/ntp_loader.{h,cpp}` — archive read (miniz), validation
- `src/plugins/ntp_graph.{h,cpp}` — manifest → graph materialisation
- `src/plugins/ntp_voices.{h,cpp}` — instrument voice pool
- `src/ui/ntp_ui.{h,cpp}` — declarative control rendering
- `src/plugins/preset_bank.{h,cpp}` — factory/user presets
- `tools/ntp-convert/` — web manifest converter
