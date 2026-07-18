# 03 — Audio Backend

## Role of OpenAL

OpenAL is the project's standard output API, but it is not a DSP graph.
The design treats it strictly as a *device*: one streaming source, all
mixing/routing/DSP performed by our own float32 graph. Nothing above
`src/audio/audio_device_openal.cpp` knows OpenAL exists.

## Pull model via AL_SOFT_callback_buffer

Primary path: OpenAL Soft's `AL_SOFT_callback_buffer` extension
(`alBufferCallbackSOFT` — present in local headers,
`/usr/include/AL/alext.h:579`). The OpenAL mixer thread calls us to fill
frames: a genuine audio callback with no polling, no queue-drain jitter.
Spec constraints (Research/02): the callback must be RT-safe (it *is*
our audio thread and lives under the same discipline), and a callback
buffer attaches to exactly one static source — which is our topology,
one master output source.

Fallback path (kept small, for exotic OpenAL implementations only): the
classic `alSourceQueueBuffers` pump polling `AL_BUFFERS_PROCESSED`.
Since OpenAL Soft is a pinned dependency, the fallback should never run
in practice; it exists so `AudioDevice` has a second implementation
proving the interface.

`AudioDevice` contract is pull-based:

```
class AudioDevice {
    // callback fills interleaved float frames; called on the device's thread
    virtual bool start(uint32_t sampleRate, RenderCallback cb) = 0;
    virtual void stop() = 0;
    virtual uint32_t actualSampleRate() const = 0;
    virtual LatencyInfo latency() const = 0;
};
```

This contract fits `AL_SOFT_callback_buffer`, `ALC_SOFT_loopback`, ALSA,
and PipeWire equally, so the backend can be swapped without touching the
graph. We do not build a second backend now.

## Block model

- Fixed internal block size: 128 frames (compile-time constant, revisit
  only with measurements). The device callback consumes whole blocks
  from a small FIFO regardless of the device's own buffer size.
- Deterministic consequences: feedback-cable latency is exactly one
  block ([06-graph-cables.md](06-graph-cables.md#feedback)); gate/CV
  edges are block-accurate; offline export is the same graph clocked by
  sample count, so export cannot drift from playback
  ([10-formats-io.md](10-formats-io.md#export)).
- Sequencer tick boundaries are computed in frames inside the block loop
  (a block may span a tick boundary; the engine splits the block at the
  boundary — see [04-engine.md](04-engine.md#transport)).

## Sample-rate strategy

- The graph runs at the device rate (request via `ALC_FREQUENCY` hint,
  then query actual; never let OpenAL resample our output).
- Sample assets are resampled once at load time (libsamplerate) to the
  graph rate; the sampler stores loop points in frames of the buffer it
  actually plays — fixing the web's fragile source-rate loop invariant
  (`Source/federated-industries-main/src/lib/trackerAudio.ts:660-668`,
  fix-list #9).
- Latency surfaced to the user: `ALC_REFRESH`/period hints where
  available; document `~/.alsoftrc` `period_size`/`periods` tuning for
  Linux in the user help.

## Master chain and channel strips

Ported from the web topology (`src/lib/trackerAudio.ts:217-251,409-559`):

- Per-channel strip: gain → pan → meter tap → pre-fader FX send → dry.
- Master: sum → compressor → optional retro section (DC blocker → retro
  lowpass → bitcrush → M/S width) → meter → device.
- All stages are ordinary graph nodes ([06-graph-cables.md](06-graph-cables.md));
  the retro section's bitcrusher is the same DSP as the native port of
  `public/audioworklets/bitcrusher.js` (phase-accumulator rate reduction
  + bit quantisation, worklet lines 44-88).

## Audio → UI feedback

Per block, the audio thread publishes a snapshot (triple buffer, `rt/`):
playhead (order/pattern/row/tick), per-channel and master meter values,
gate/CV values for cable visualisation, xrun counter, measured latency.
The debug overlay ([13-verification.md](13-verification.md)) renders
these; underruns must be observable, never silent.

## File map

- `src/audio/audio_device.h` — the pull contract above
- `src/audio/audio_device_openal.cpp` — callback-buffer primary,
  queue-pump fallback
- `src/audio/graph_runner.{h,cpp}` — block loop, tick splitting, FTZ/DAZ
- `src/audio/channel_strip.{h,cpp}`, `src/audio/master_chain.{h,cpp}`
- `src/audio/snapshot.h` — the audio→UI block snapshot type
