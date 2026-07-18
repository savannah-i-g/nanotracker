# 08 — External Plugin Hosting (CLAP, VST3)

## Locked decision

CLAP hosting lands first, VST3 follows within v1 scope. Project license
is GPLv3 from day one; `include/ntp/` stays MIT with an explicit plugin
exception (decisions locked 2026-07-17, [00-index.md](00-index.md)).

Rationale recorded for posterity:

- Research finding (Research/04): Steinberg relicensed the VST3 SDK to
  **MIT in November 2025**, so no dependency forces copyleft. GPLv3 was
  re-put to the project owner with that premise removed and **reaffirmed
  on copyleft merits** (2026-07-17): anyone building on NanoTracker
  shares source back — the norm among open-source DAWs (Ardour, Zrythm).
- CLAP (MIT, single C header, sane threading contract) proves every
  piece of "external plugin as patch node" plumbing — parameter
  exposure, state chunks, editor windows, MIDI routing — at a fraction
  of VST3's ceremony (IComponent/IEditController split, connection
  points). VST3 then reuses that plumbing. Both SDKs are now permissive,
  so ordering is purely an engineering choice.
- NTP plugin authors are never GPL-contaminated because the NTP headers
  are MIT.

## Integration shape

External plugins are graph nodes like any other
([06-graph-cables.md](06-graph-cables.md)):

- Audio ports map to jacks; parameters are exposed for CV cables and
  MIDI-learn exactly like NTP params; note events arrive via midi-kind
  cables and the tracker instrument table.
- State: opaque plugin state chunks stored in the FTRK external-plugin
  block ([10-formats-io.md](10-formats-io.md)), keyed by stable plugin
  IDs, with the parameter snapshot alongside for degraded restore when
  the plugin is missing.
- Processing runs on the audio thread inside the block schedule; CLAP's
  threading contract fits directly. Plugins that require a larger block
  granularity are wrapped with internal buffering.

## Editor windows

Plugin GUIs are **separate OS windows**, never embedded in ImGui:

- Concrete Linux mechanics (Research/05): the host creates one
  top-level X11 window per open editor; the plugin embeds its own X11
  window into it (CLAP: XEmbed via `_XEMBED_INFO`; VST3: `IPlugView`
  attached to the window). The host implements CLAP's
  `posix_fd_support` + `timer_support` extensions (VST3: `IRunLoop`) so
  plugin GUIs receive their display-fd callbacks — without these,
  editors starve on Linux. Wayland embedding does not exist in either
  standard; under a Wayland session these X11 windows run through
  XWayland, consistent with the platform policy
  ([02-architecture.md](02-architecture.md#platform-policy)).
- GL state: in-process plugin GUIs may clobber GL state; the host
  saves/restores context state around plugin UI calls. Out-of-process
  bridging is noted as future hardening, not v1.
- **Auto-param panel**: every external plugin also gets a generated
  ImGui panel (sliders/combos from its parameter list) — the fallback
  when an editor is unavailable, headless, or misbehaving, and the same
  renderer NTP's declarative UI uses
  ([07-plugins-ntp.md](07-plugins-ntp.md)).

## Crash posture

In-process plugins can take down the host. Mitigations in v1: strict
load-time validation, crash-recovery autosave journal
([10-formats-io.md](10-formats-io.md#autosave)), and a
last-loaded-plugin marker so a crash on next launch can offer to skip
the offender. Process isolation is out of v1 scope and recorded as
future hardening.

## File map

- `src/ext/clap_host.{h,cpp}` — CLAP hosting (first)
- `src/ext/vst3_host.{h,cpp}` — VST3 hosting (second, same node shape)
- `src/ext/ext_node.{h,cpp}` — shared "external plugin as graph node"
- `src/ext/editor_window.{h,cpp}` — OS window management for editors
- `src/ui/auto_param_panel.{h,cpp}` — generated parameter UI (shared
  with NTP webview fallback)
