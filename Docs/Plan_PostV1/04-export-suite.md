# 04 — Export Suite (Stage 15)

The highest-severity parity gap: v1 export renders faithfully but
offers no options. This stage is options + post-processing + encoder
knobs + a real window; the offline engine run itself
(`src/io/export_render.*` — serialise → fresh offline session →
sample-clocked render) is done and stays the spine.

Behavioural references: `lib/exportRenderOffline.ts`
(`OfflineRenderOptions:29`), `lib/exportPostProcess.ts`,
`lib/exportPresets.ts`, `lib/exportMetadata.ts`,
`components/TrackerExportModal.tsx`.

## Render options

- Order range (`startOrder` / `endOrder`) — the offline transport
  seeds `order_pos` and ends at range end instead of song wrap.
- Tail seconds (exists; becomes user-facing).
- Stems: a channel mask renders one pass per enabled channel (or one
  multi-pass run) into per-channel files; ZIP bundling of the result
  (miniz writer is already in-tree via NTP).
- Sample rate choice (render rate independent of device rate — the
  offline engine already takes a rate parameter).

## Formats

- WAV 16-bit (exists) + 24-bit + 32-float writers.
- OGG quality (vorbis VBR −0.1..1.0; currently fixed 0.5).
- MP3 bitrate/preset (lame CBR/VBR knobs through the existing
  dlopen'd shim).

## Post-processing (applied to the rendered float buffer)

- Fade in / fade out (linear + equal-power).
- Peak normalize; true-peak normalize; LUFS normalize per ITU-R
  BS.1770-4 (integrated loudness) — all three via **libebur128**
  (MIT, conformance-passing; true-peak scanning included, so the
  trio costs one dependency; no self-implementation — Research/05,
  applied 2026-07-18).
- Metadata tags: WAV INFO/LIST, Vorbis comments, ID3 via lame.

## Presets + UI

- Export presets: named option bundles (built-ins matching the web's
  + user presets in the config dir, same JSON discipline as the
  preset bank).
- A dedicated export window replacing the shell path box: format,
  options, post-processing, metadata, presets, progress readout
  (export runs on a worker thread; the offline engine is already
  device-independent).

## File map

- `src/io/export_render.{h,cpp}` — options struct grows; stems loop
- `src/io/export_post.{h,cpp}` — fades + normalizers (new)
- `src/io/export_presets.{h,cpp}` — preset store (new)
- `src/ui/export_view.{h,cpp}` — the window (new)

## Verification

Unit tests per option: order-range → rendered length matches the
range's tick count; stems → per-channel spectral checks (the
audible-fixture channels carry distinct frequencies); bit depths →
WAV headers + round-trip decode; LUFS → known-level fixtures land
within ±0.5 LU; metadata → tags read back by our own decoders where
possible. UI via input-script + screenshot.
