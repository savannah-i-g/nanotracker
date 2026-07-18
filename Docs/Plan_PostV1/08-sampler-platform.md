# 08 — Sampler Platform (Stage 19)

Pays the two sampler debts that make shared `.ftrk` files play
differently than authored.

## POVR consumption — user-assignable slots go live

The web feature (`lib/pluginSampleOverrides.ts` end to end;
`pluginTypes.ts:428` `userAssignable`, `:435` `slotId`): a plugin
zone marked user-assignable exposes a slot the user drops their own
sample into; overrides persist per instance, keyed by stable slot id
and content hash. Natively today the POVR block round-trips verbatim
(`io/ftrk_reader.cpp:495`, `ftrk_writer.cpp:315`) but nothing reads
it — the feature is dead.

- Parse POVR properly (per-instance, per-slot: slotId → hashed blob
  + metadata); the raw passthrough remains the fallback for unknown
  sub-records.
- `plugins/ntp_loader` honours `userAssignable`/`slotId`/
  `fallbackFile` in zones; `ntp_graph` sampler resolves a slot's
  buffer through an instance-level override table before the baked
  archive sample.
- Override table on `NtpInstance`: set/clear at runtime (structural:
  buffers swap through the Stage 16 reclamation path).
- Slot picker UI in the plugin window body (`ui/ntp_ui.cpp`): slot
  list, current assignment, pick-file, clear-to-fallback.
- Writer emits POVR from the live override table (content-hash keyed,
  matching the web's shape so files stay cross-compatible).

## sliceMap — MPC-style chopping

Web reference: `pluginTypes.ts:465-542` (`PluginSliceEntry`,
`PluginSliceMap`), gated/one-shot trigger modes, slice choke/RR,
`autoDetect: "grid:N"` (the web's "transients" detector fell back to
grid — port the honest fallback, record the detector as parked).

- Manifest: `sliceMap` on sampler nodes (source, slices[],
  autoDetect, triggerMode) — loader validation + grid expansion.
- Runtime: slice triggering by note (default `36 + index`), per-slice
  start/end, gated release, slice choke/round-robin sharing the
  existing zone group machinery (`ntp_graph.cpp` interned groups).
- ntp-convert carries web sliceMaps across unchanged.

## Verification

NTP fixture archives grow: a sliced-break fixture (grid:8 over a
click pattern — slice boundaries spectrally distinguishable) with
trigger tests per mode; an override fixture where a test swaps a
slot and the rendered output's frequency changes accordingly; POVR
round-trip: save with overrides → reload → same audio (hash-keyed
buffer equality).
