# 11 — MIDI

## Scope

Port of the web's WebMIDI feature set (`Source/federated-industries-main/
src/lib/trackerMidi.ts`, `midiBus.ts`, `midiClockOutput.ts`,
`trackerEffectToMidi.ts`, `midiMappings.ts`) on RtMidi:

- **Input**: note entry (live + record into patterns), controller input,
  MIDI-learn for any learnable parameter (NTP params, external plugin
  params, mixer controls) — mappings persisted in settings
  ([10-formats-io.md](10-formats-io.md#settings)).
- **Output**: Ext MIDI Out pseudo-node in the patch graph
  ([06-graph-cables.md](06-graph-cables.md)) routes midi-kind cables to
  hardware; tracker effect→MIDI translation ports
  `trackerEffectToMidi.ts` semantics.
- **Clock out**: 24 PPQN MIDI clock, start/stop/continue, song position.

## Timing design

The one place the web is structurally better off than a naive native
port: block-quantised sends from the audio thread would jitter by the
block size (128 frames @ 48kHz ≈ 2.7ms — audible slop on hardware
sequencers), and RtMidi sends immediately with no timestamping.

Design: a dedicated MIDI output thread runs a phase-locked loop against
audio sample time (the audio thread publishes block-start sample time +
host time in the snapshot; the MIDI thread interpolates). Events are
queued with target timestamps and dispatched at high resolution,
independent of block boundaries. Reliable timing is the thesis of this
port; the MIDI clock must honour it.

The 24 PPQN position bookkeeping ports from the web transport
(`transportClock.ts` ppq24 counters) as plain counters advanced by the
sequencer ([04-engine.md](04-engine.md#transport)).

## Routing model

MIDI is a first-class cable kind, not a side channel: hardware inputs
appear as Ext MIDI In pseudo-nodes, plugins receive midi-kind cables
(implicit midi-in ports, [07-plugins-ntp.md](07-plugins-ntp.md)), and
thru behaviour follows the web's opt-out flags
(`pluginTypes.ts:949-957`). The web's separate `MidiBus` singleton
disappears into the graph.

## File map

- `src/midi/midi_io.{h,cpp}` — RtMidi device management, input thread
- `src/midi/midi_out_thread.{h,cpp}` — PLL scheduler, clock generator
- `src/midi/midi_learn.{h,cpp}` — learn state machine + mapping store
- `src/graph/pseudo_nodes/ext_midi.{h,cpp}` — graph endpoints
