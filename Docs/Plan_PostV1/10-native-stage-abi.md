# 10 — native_stage C-ABI + GA Release (Stage 21)

Deliberately last: the ABI freezes only after Stage 17 (runner port
semantics) and Stage 20 (DSP internals) stop moving what it would
freeze. Freezing earlier ships ABI v2 within the same cycle — worse
in public, where third parties build against v1 the week it appears.

## The C-ABI

The v1 reservation comes due: `native_stage` nodes
(`include/ntp/ntp_manifest.h:49`, refused by the loader since
Stage 9) become real.

- `include/ntp/ntp_stage_abi.h` (MIT + plugin exception, like the
  manifest header): a versioned C struct API — `abi_version` field
  from day one, process callback (float32 stereo blocks, frames,
  param array), param descriptor table, state save/load chunk,
  create/destroy. No UI in the ABI (declarative UI stays the NTP
  surface; the stage is DSP only).
- Loader: `native_stage` nodes name a shared library in the archive
  (per-platform subdirs, the CLAP model); dlopen/LoadLibrary through
  the Stage 13 shared-library wrapper; strict validation — missing
  binary for the host platform = clean refusal listing platforms
  present.
- Runtime: a NodeRuntime kind that trampolines process() into the
  stage; params bridge through the existing ParamSlot machinery.
- Security posture stays explicit: unlike pure-data NTP, an archive
  with a native stage executes code — the loader labels these
  archives distinctly in the UI (trust decision surfaced to the
  user, never silent).

## GA release

- Windows beta → GA per locked decision 4 (community confirmation
  through the public repo; the criteria and current status live in
  the README).
- Packaging (Research/06, applied 2026-07-18): Linux **AppImage**
  primary — the Flatpak sandbox fights a plugin host (system
  CLAP/VST3 dirs, native_stage dlopen); Flatpak parked as a
  community follow-up. Windows: portable **zip** (same shape as the
  Stage 13 beta artifact) + a winget manifest submitted at GA.
- Release notes generated from PROGRESS.md; the site's features page
  points at real downloads; git tags become the release backbone
  (tar backups continue).

## Verification

ABI: a fixture native stage built in-tree (like the CLAP test
plugin) — load, process (known DSP: gain), state round-trip, param
sweep; a *deliberately wrong-version* fixture asserting the refusal
path. Cross-platform CI builds the fixture on both runners.
Packaging: the packaged artifact boots and passes an input-script
smoke run on CI where the format allows.
