# Research — OpenAL Soft Callback Buffer and Device Rate

## Findings

| Fact | Value |
| --- | --- |
| Extension | `AL_SOFT_callback_buffer` (`alBufferCallbackSOFT`) |
| Model | OpenAL Soft's mixer thread pulls samples from the callback when mixing needs them — no extra buffering latency |
| RT contract | callback must be real-time safe: no blocking, I/O, allocation; bounded completion time |
| Source constraint | a callback buffer may only be set on ONE static source via `alSourcei(sid, AL_BUFFER, bid)`; queueing it or using a second source raises `AL_INVALID_OPERATION` |
| Callback signature | `ALsizei cb(ALvoid* user, ALvoid* sampledata, ALsizei numbytes)` |
| Device rate | `ALC_FREQUENCY` context attribute is a request, not a guarantee; query the actual value back via `alcGetIntegerv(device, ALC_FREQUENCY, ...)` |

## Plan implications

- Confirms the pull-model `AudioDevice` design
  ([../03-audio-backend.md](../03-audio-backend.md)): one static source
  carrying the callback buffer, graph rendered inside the callback.
- The single-static-source constraint is exactly our topology (one
  master output). No design change needed; the constraint is now
  documented where the device backend will live.
- The request-then-query rate flow matches the "run graph at device
  rate" strategy; the graph reads the *queried* rate, never the
  requested one.
- The RT-safety contract on the callback is the same discipline already
  mandated for the audio thread ([../02-architecture.md](../02-architecture.md));
  the callback simply is the audio thread.

## Sources

- [SOFT_callback_buffer extension spec](https://openal-soft.org/openal-extensions/SOFT_callback_buffer.txt)
- [OpenAL Soft issue #325 (on-demand source buffers → extension rationale)](https://github.com/kcat/openal-soft/issues/325)
- [OpenAL Soft issue #350 (querying device sample rate)](https://github.com/kcat/openal-soft/issues/350)
- [OpenAL Soft issue #248 (forcing frequency — request not guaranteed)](https://github.com/kcat/openal-soft/issues/248)
