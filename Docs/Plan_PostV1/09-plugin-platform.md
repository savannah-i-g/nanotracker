# 09 — Plugin Platform (Stage 20)

Three revisits recorded in FIXES.md/PROGRESS plus the VST3 editor
gap — grouped because they mature the plugin surface without
touching the ABI (which deliberately freezes only after this stage).

## Partitioned-FFT convolution engine

One engine, two payoffs (both logged as revisits):
- NTP convolver uncapped (currently direct FIR, 2048 taps ≈ 43ms,
  `plugins/ntp_graph.h kMaxImpulseFrames`).
- FX-mixer REVERB gains a convolution mode for web parity (currently
  Freeverb topology — the FIXES.md "deliberate divergence" entry
  gets its promised revisit).

Design: uniform partitioned convolution first (block-size partitions,
frequency-domain accumulate — RT-safe, zero-latency at 128 frames),
non-uniform scheme only if profiling demands it. FFT library:
**pffft**, marton78 fork (BSD-3-like, SIMD, real-input transforms);
FFTConvolver (MIT) is the validated design reference for the uniform
two-stage scheme (Research/04, applied 2026-07-18). Lives in
`src/audio/dsp_fft.{h,cpp}` + `convolution_engine.{h,cpp}` shared
by both call sites, with pffft wrapped behind our own interface.

## Sprite animation keys

Web `lib/pluginSprite.ts`: named animations at 10fps, triggered by
interaction keys. Native `ui/ntp_ui.cpp` renders frame 0 statically.
- Manifest: animation defs on sprite assets (frames, fps, loop).
- Renderer: frame advance on ImGui time; `animation` key triggers
  one-shots on control interaction.

## Envelope editor becomes an editor

`ui/ntp_ui.cpp kEnvelopeEditor` is display-only. Add stage-point
dragging (target/time), writing back through the instance to the
node's env stages (structural republish — stage arrays are read by
voices; same discipline as sequence notes).

## VST3 editor windows

IPlugView attached to the Stage 13 platform editor-window
abstraction; host-side IRunLoop implementation mirroring what the
CLAP fd/timer pump already does, with the contract pinned by
Research/07 (applied 2026-07-18): callbacks MUST fire on the UI
thread (JUCE plugins assume it — never a worker poll thread, which
matches our frame-loop pump); expose IRunLoop both via the
IPlugFrame passed to `setFrame` and via
`IPluginFactory3::setHostContext`; guard handler lists against
unregister-during-dispatch. Win32 needs no IRunLoop (global message
loop) and rides the same abstraction. Auto-param panel remains the
fallback.

## Verification

Convolution: null test against direct FIR at short IRs (identical
output within float tolerance); long-IR decay behaviour spectral
check; RT-safety under the debug allocator. Sprites/envelope:
fixture plugin + screenshots (animation = two frames differ across
shots). VST3 editor: TyrellN6.vst3 opens a WM-managed window (same
read-only X-tree check as the CLAP editor verification).
