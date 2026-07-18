# FTRK File Format

Normative description of the `.ftrk` project format as read and
written by native NanoTracker. The web serializer
(`Source/.../src/lib/trackerSerializer.ts`, `binarySerializer.ts`)
defined versions 1-13; the native writer emits **version 14**, whose
single addition is the XPLG block and a wider reserved header region.
The native reader accepts 1-14.

All integers are **little-endian**. Fixed-width strings are
zero-padded; readers trim at the first NUL. Length-prefixed strings
are `u8`/`u16`/`u32` byte counts followed by raw UTF-8.

## Header

| Field | Type | Notes |
| --- | --- | --- |
| magic | 4 bytes | `FTRK` |
| version | u16 | 14 written; 1-14 read |
| name | char[32] | project title |
| bpm | f32 (v2+) / u16 (v1) | |
| speed | u8 | ticks per row |
| rowsPerPattern | u8 | default rows (v6+ patterns carry their own) |
| channels | u8 | |
| patternCount | u8 | |
| orderLength | u16 | |
| sampleCount | u8 | |
| reserved | 31 bytes (v2-13), **35 bytes (v14+)**, 35 bytes (v1) | block offsets below |

Block offsets inside the reserved region (u32 file offsets, 0 = block
absent):

| Offset | Block | Since |
| --- | --- | --- |
| +0 | FXMX | v4 |
| +4 | INTB | v5 |
| +8 | WPBR | v7 |
| +12 | PLGB | v8 |
| +16 | SEQB | v10 |
| +20 | POVR | v12 |
| +24 | PPRS | v13 |
| +28 | XPLG | **v14** |

The v14 region grew from 31 to 35 bytes to fit the eighth offset; the
remaining 3 bytes stay reserved. Pre-v14 readers refuse the version
loudly — the intended failure mode for files carrying features they
cannot represent.

## Core sections (immediately after the header)

1. **Order list** — `orderLength` × u8 pattern ids.
2. **Patterns** — per pattern: name char[16]; v6+: rows u16,
   highlightMajor u8, highlightMinor u8; then rows × channels cells of
   5 bytes: note (0 empty, 1-96, 97 release), instrument byte
   (v9+: slot in bits 0-4, bound sub-slot in bits 5-7), volume
   (0xFF default), effect, effectParam.
3. **Sample directory** — per sample: id u8, format u8 (0 wav / 1 ogg /
   2 mp3), baseNote u8, finetune i8, volume u8, pan u8, loopStart u32,
   loopLength u32 (source-rate frames), sampleRate u32, numChannels u8,
   frames u32, dataLength u32, name char[22], fileName char[32],
   stretch u16 (ratio × 10000; 0 = 1.0, v2+), category u8 (v3+).
4. **Sample payloads** — the original encoded file bytes, concatenated
   in directory order. Samples persist as their source files; decode
   happens at load.

Core sections fail loudly on corruption. Every appended block below is
individually tolerant: damaged blocks are reported and skipped, the
song still loads.

## Blocks

### FXMX — FX mixer
`FXMX`, channelCount u8; per channel: name char[16], volume u8,
pan i8, enabled u8, colour str8 (`#rrggbb` or empty), sendCount u8 ×
f32, moduleCount u8 × { moduleId char[16], enabled u8, paramCount u8 ×
{ key str8, value f32 } }. Then fxPatternCount u8; per pattern:
rowCount u16 rows of either u8 0 (empty) or u8 1 + { channel u8,
module u8, paramKey str8, value f32 }.

### INTB — instrument table (+BNDT)
`INTB`, entryCount u8; per entry a type byte: 0 sample → sampleId u8;
1 plugin → pluginId str8 + paramCount u8 × { key str8, value f32 };
2 workspace → workspaceId str16. v9+ appends `BNDT`: per entry
count u8 × track u8 (bound tracks, sorted/deduped on read).

### WPBR — workspace patchbay
`WPBR` + str32 JSON (`WorkspaceProjectState`: instruments with window
snapshots and volume/pan/bypass, cables with stable `portId` alongside
legacy `jackIndex`). Native adoption rules live in
`src/graph/graph_wpbr.h`.

### PLGB — bundled NTP plugins
`PLGB`, count u16; per plugin: pluginId str16, archive u32 bytes (the
original `.ntins`/`.ntsfx` ZIP).

### SEQB — sequence layers + channel colours
`SEQB`, patternCount u16, channelCount u8; per pattern × channel:
layerCount u8 × { instrument u8, enabled u8, noteCount u16 × { pitch
u8, startTick u16, durationTicks u16, velocity u8 } }. Trailer:
colourCount u8 × { channel u8, r u8, g u8, b u8 } (sparse custom
channel colours).

### POVR — plugin sample overrides
`POVR`, blockVersion u8 (1), instanceCount u16; per instance:
instanceId str16 (plugin instance workspace id), slotCount u16; per
slot: slotId str16, hash str16 (`sha256:<hex>` of the sample bytes),
name str16 (original filename), sampleRate u32, channels u8,
duration f32 (seconds), byteLen u32 + bytes (the original encoded
file). Dedup: each unique hash's bytes appear once; later references
carry byteLen 0 and readers re-use the first occurrence. Identical to
the web v12 layout, so overrides stay cross-compatible.

The native host parses the block into per-instance override tables
(slots resolve override-first, baked `fallbackFile` second). Unknown
block versions — and entries whose instance never resolves at load —
round-trip untouched.

### PPRS — project plugin presets
`PPRS`, blockVersion u8 (1), str32 JSON — an array of
`{ instanceId, activePresetId?, projectPresets[] }` records
(`UserPresetRecord` shape).

### XPLG — external plugin state (v14, native)
`XPLG`, count u16; per instance: workspaceId str16, kind u8 (0 CLAP,
1 VST3), pluginId str16 (CLAP id / VST3 class UID), libraryPath
str16, state u32 bytes (CLAP state stream; VST3: u32 component-stream
length + bytes + u32 controller-stream length + bytes), paramCount
u16 × { id u32, value f64 } — the parameter snapshot for degraded
restore when the plugin is missing on load.

## Compatibility notes

- The native writer always emits v14 with every block its project
  needs; blocks with nothing to say are omitted (offset 0).
- Loop points and 9xx offsets are stored in **source-rate frames**
  (web compatibility); the native sampler converts to resident-buffer
  frames at voice trigger (FIXES.md #9).
- The web reader will refuse v14 files by version check. Projects
  meant for the web must come from the web; the native app is the
  format's forward home.
