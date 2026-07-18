# 06 — MIDI Completion (Stage 17)

Wakes everything the v1 MIDI stage left dormant. After this stage,
midi-kind cables are real signals, hardware can author patterns, and
row effects drive external gear expressively.

## Runner midi-edge transport

The named first post-v1 work item (PROGRESS). Model on the gate
transport (`src/audio/graph_runner.cpp` GateEventList):

- Per-block midi event lists (note on/off + CC, frame-stamped) as a
  port buffer type; kMidi edges carry them (compiler stops dropping
  them at `graph_compile.cpp:82`).
- Tracker bus midi ports wake: `chNN.midi` outs (row events per
  channel) + `master.midi` + `master.midi.in`, matching the web port
  ids so dormant WPBR cables resolve (`graph_wpbr.cpp` adoption
  already maps them — they simply stop being dormant).
- Ext MIDI In / Ext MIDI Out pseudo-nodes (`graph::NodeKind` grows):
  In bridges the `midi::MidiInput` ring into graph events; Out
  queues graph events onto `midi::MidiOutThread` (timestamped, PLL
  dispatch — the thread exists).
- Plugin nodes with midi-in ports receive cable events through their
  bindings (`GraphPluginBinding::plugin_note_on/off`; CLAP dialect
  negotiation already handles u-he-style plugins).

## CV → plugin host-params

The runner routes CV blocks but kPlugin nodes ignore CV-ins
(`graph_runner.cpp:257`). Wire: CV input port value (block average +
slew, consistent with NTP mod routes) → instance parameter. Target
selection: the cable's dest port id names the param (NTP dot-path /
CLAP/VST3 numeric id) — ports generated per-instance from the
parameter list (bounded count; the workspace node grows a "CV ports"
section like the web's webviewExposable set).

## Pattern record + step entry

Behavioural reference: `components/TrackerMidiPanel.tsx` modes
(preview | enter | record):
- Step entry: inbound note writes the cell at the cursor and
  advances (exact parity with keyboard entry, routed through
  `set_cell` → undoable).
- Live record: during playback, inbound notes write cells at the
  playhead row of the record-armed channel (quantized to row).
- Mode + armed-channel UI in `ui/midi_view.cpp`.

## Effect→MIDI translator

Port `lib/trackerEffectToMidi.ts` semantics: Dxx→CC7 delta,
Exx/Fxx/Gxx→pitch-bend, Mxx/Vxx→CC7 absolute, Xxx→CC10 pan,
Sxx→noteOff, Txx→BPM (clock period). Runs where row context lives:
the sequencer emits channel row events into the tracker-bus midi
ports (above), so the translation is a graph-side producer — cables
decide where it goes (Ext MIDI Out for hardware).

## Verification

Extend the live ALSA loopback suite (`tests/midi_test.cpp`):
- Cable transport: virtual-port loop Ext In → cable → Ext Out;
  event lists asserted per block via runner debug accessors.
- Effect→MIDI: scripted pattern with known effects → captured byte
  stream asserted (CC/pitch-bend values exact).
- Record: inject virtual-port notes during playback, diff cells.
- CV→param: cable a bus vol CV to an NTP param, assert param motion.
