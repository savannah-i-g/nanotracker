# 01 — Context

## Why this port exists

NanoTracker is a browser-based music tracker (FastTracker lineage:
patterns, channels, instruments, hex effect commands, order list) inside
the federated-industries site repository. The browser imposes limits the
app has visibly fought: audio scheduling through a lookahead pump and an
AudioWorklet clock, tracker-module playback approximated through lossy
import, plugin DSP restricted to declarative WebAudio graphs, windows and
patch cables tracked through per-frame DOM queries. A native build removes
every one of those ceilings. Long-term, the native app becomes the default
NanoTracker and the hosted site shrinks to a features page.

## The source application

Web root: `Source/federated-industries-main/` (relative to the port
workspace `~/Documents/Ports/NanoTracker/`). Route `/tracker` →
`src/pages/Tracker.tsx` (3,599 LOC). NanoTracker-specific code ≈ 38K LOC
TS/TSX + ~3.5K CSS across ~90-100 files. Subsystem detail lives in the
sibling docs; headline anatomy:

- Pure sequencer state machine `src/lib/trackerEngine.ts` — see
  [04-engine.md](04-engine.md).
- Binary project format `.ftrk` v13 `src/lib/trackerSerializer.ts` +
  `src/lib/binarySerializer.ts` — see [10-formats-io.md](10-formats-io.md).
- Declarative ZIP+JSON plugin system (~11.5K LOC) — see
  [07-plugins-ntp.md](07-plugins-ntp.md).
- Typed patch-cable graph over instrument windows — see
  [06-graph-cables.md](06-graph-cables.md).
- Custom window manager (two families, phantom-window workarounds) — see
  [09-windows-ui.md](09-windows-ui.md).
- WebAudio engine + 5 AudioWorklets — see
  [03-audio-backend.md](03-audio-backend.md) and
  [04-engine.md](04-engine.md).
- Hand-written MOD/XM/S3M/IT importers, lossy by design — see
  [05-module-playback.md](05-module-playback.md).

Out of scope from the same repo: marketing pages, MapManager, DataBeat,
PWA/service worker, Firebase fetching (sprite assets move into plugin
archives).

## Fix-don't-retain list

Broken or patchy web behaviours that get fixed, not ported. Each entry
names the web evidence; the fix's design home is linked. When a fix
lands, it is recorded in `Docs/FIXES.md`.

1. XM relNote sign-bug lineage; XM version field unvalidated
   (`src/lib/xmImporter.ts:330`, `:392`) → [05](05-module-playback.md).
2. Lossy import mappings silently degrade songs: S3M Tremor dropped
   (`s3mImporter.ts:47`), IT Tremor/Panbrello approximated
   (`itImporter.ts:62,82`), XM Tremolo approximated, unknown effects
   zeroed (`xmImporter.ts:50,58`) → [05](05-module-playback.md).
3. Hardcoded 40-byte XM header (`xmImporter.ts:264`) →
   [05](05-module-playback.md).
4. Sampler decode-not-ready paths silently play a null buffer
   (`samplerRuntime.ts:212-244`) → [04](04-engine.md).
5. Local API gaps: no sample upload, bogus IDs accepted silently
   (`trackerLocalApiSchema.ts:104-107`) → out of v1 scope; recorded so
   the native settings/API design does not inherit it.
6. Dead migration cruft: settings that never existed fall through to
   hardcoded defaults (`TrackerSettingsWindow.tsx:97`; comment scars
   `Tracker.tsx:3080,3455`) → [10](10-formats-io.md).
7. Export capture via deprecated ScriptProcessorNode
   (`trackerAudio.ts:1055-1088`) → [10](10-formats-io.md): offline render
   is a sample-clocked run of the same graph.
8. S3M ADPCM unsupported, pingpong loops partial (`s3mImporter.ts:173-178`,
   `TrackerHelp.tsx:1280`) → [05](05-module-playback.md): moot for
   playback once libopenmpt owns standard modules; importer gets real
   pingpong.
9. Loop points stored in source-rate frames while playback uses a
   context-rate buffer (`trackerAudio.ts:660-668` vs
   `modImporter.ts:242-243`) → [04](04-engine.md): native sampler stores
   loop points in frames of the buffer it plays.
10. Plugin-graph reschedule glitch workarounds; "degraded" fallback for
    broken plugin schemas (`pluginGraphBuilder.ts:137,226`,
    `pluginInstrumentGraph.ts:562`) → [07](07-plugins-ntp.md): strict
    load-time validation.
11. Gate cables polled at ~60Hz via analyser RMS
    (`workspaceCableGraph.ts:107-114`) → [06](06-graph-cables.md):
    gate/CV are real block-accurate signals.
12. Cable endpoints resolved by per-frame DOM queries; phantom windows and
    minimised jack-strips exist only to keep anchors alive
    (`CableOverlay.tsx:83,218`, `InstrumentWindowPhantom.tsx`) →
    [06](06-graph-cables.md), [09](09-windows-ui.md): anchors come from
    the app's own window registry.
13. Failed cable connections only `console.warn`
    (`workspaceCableGraph.ts:284-388`) → [06](06-graph-cables.md):
    visible user feedback.
14. Dual endpoint addressing (portId vs jackIndex), drag commit passes
    only the index (`CableOverlay.tsx:322`) → [06](06-graph-cables.md):
    stable string port IDs everywhere.
15. Legacy dual port model + plugin schema v1-v5 migration debt
    (`pluginInsAdapter.ts:94-113`) → [07](07-plugins-ntp.md): single
    typed port model, single manifest version.
16. Self-feedback cables banned as a stopgap (`CableOverlay.tsx:319`) →
    [06](06-graph-cables.md): feedback allowed via one-block delay.

## Related docs

Architecture spine: [02-architecture.md](02-architecture.md).
Doctrine and workspace conventions: [12-doctrine.md](12-doctrine.md).
