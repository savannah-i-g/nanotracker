# 01 — Out-of-Process Plugin Bridge (Stage 29) — Design + Research

Design and research pass for the heaviest Cycle 3 item
([00-plan.md](00-plan.md#stage-29--out-of-process-plugin-bridge)). No
production code lands with this document; it exists so the build lands
incrementally with a defensible real-time story. The one thing written
during this pass was a throwaway feasibility probe (recorded in §A.5).

## Goal, restated

External CLAP/VST3 instances host in a **child process** so a plugin
crash cannot take the tracker down. In-process stays the default;
bridging is **per-plugin opt-in**. A crashing plugin's death →
auto-bypass (the node goes quiet or passes dry), a loud node badge, and
one-click restart, **with audio and the session surviving**. This is the
"future hardening" the v1 external-plugin doc explicitly deferred
([../Plan_NativePort/08-external-plugins.md](../Plan_NativePort/08-external-plugins.md)
lines 57, 64-71).

## What the port already gives us (and why the boundary is cheap)

The bridge is a new backing for one existing seam, not a new subsystem.

- **One narrow audio-thread interface.** Every plugin kind — NTP,
  CLAP, VST3 — reaches the graph through `audio::GraphPluginBinding`
  (`src/audio/graph_runner.h:81-109`): `process_block(in, out, frames)`,
  `plugin_note_on/off`, `plugin_set_param_cv`, `plugin_reset`.
  Interleaved stereo, `frames <= kGraphBlockFrames` (= 128,
  `graph_runner.h:40`). The runner drives a plugin node by staging notes
  and CV, then calling `process_block` synchronously
  (`graph_runner.cpp:388-443`). A **bridge is just another
  `GraphPluginBinding`** whose `process_block` talks to a child instead
  of calling `plugin_->process()` directly.
- **The hosting code is already isolated.** `src/ext/clap_host.cpp`,
  `vst3_host.cpp`, the editor windows, `editor_host_surface_*`,
  `vst3_run_loop.cpp` depend on the plugin SDKs and the OS window layer,
  not on the engine/UI. The child executable **links these files
  unchanged** and drives them exactly as the in-process host does today.
  The child *is* a NanoTracker plugin host with everything else removed.
- **A versioned-process-boundary precedent exists.** The native-stage
  ABI (`include/ntp/ntp_stage_abi.h`) already sets the discipline the
  bridge mirrors: `abi_version` is the first field and is checked before
  any other field is read (`ntp_stage_abi.h:38,138`); growth is additive
  behind reserved fields (lines 52-57); the main/audio threading split
  is written into the contract (lines 41-49). The bridge's shared-memory
  control block and control-channel handshake copy this shape.
- **RT discipline is mechanically enforced.** Debug builds route every
  `operator new`/`delete` through an abort-on-alloc check while an
  `rt::RtScope` is live on the thread (`src/rt/rt_assert.cpp:50-100`);
  the doctrine forbids allocation, locks, and syscalls on the audio
  thread ([../Plan_NativePort/12-doctrine.md](../Plan_NativePort/12-doctrine.md)
  line 17). The bridge's audio-thread half must survive this unchanged —
  which is the whole difficulty, and the subject of §A.
- **Lock-free primitives are in the tree.** `rt::SpscQueue`
  (`src/rt/spsc_queue.h`) is a power-of-two, acquire/release, POD-only
  SPSC ring; the bridge's shared-memory ring is the same discipline with
  the indices living in shared memory instead of the heap.
- **State persistence already round-trips external plugins.** The XPLG
  block stores each instance's opaque state chunk + a parameter snapshot
  for degraded restore (`project_session.cpp:298-326` writes,
  `228-257` reads; [../ftrk-format.md](../ftrk-format.md) line 124). The
  bridge reuses this verbatim for crash-restart (§C).

---

## A. IPC transport & RT safety — the load-bearing decision

The audio callback runs one 128-frame block every ~2.67 ms at 48 kHz. It
must hand that block to another process and get audio back **without ever
blocking unboundedly**, under every child state including *slow*, *hung*,
and *dead*. This section has to convince a skeptical RT engineer, so it
states the rule, the mechanism, and the failure behaviour separately.

### A.1 The rule

> The host audio thread performs only **wait-free** shared-memory ring
> operations. It never spins on the child, never enters the kernel, and
> never waits on a lock or futex. Its worst case under any child state is
> a fixed, tiny amount of work followed by *output silence for that
> block*.

Everything below serves that rule.

### A.2 Mechanism: a one-block SPSC pipeline in shared memory

The host and child are **decoupled by lock-free single-producer
single-consumer block rings in shared memory** — one host→child (audio
in + events), one child→host (audio out). They do **not** rendezvous per
block. Each audio callback the host:

1. **Pushes** this block's input (audio + notes + param changes +
   transport) into the input ring — wait-free; if the ring is full
   (child hung), the push is *dropped*, never retried, never blocked.
2. **Pops** the oldest ready output block from the output ring — wait-
   free; if the ring is empty (child not ready yet), the host writes
   **silence** for this node's output and returns.

The child runs its own real-time thread that continuously pops input,
calls the plugin's `process()` (via the reused `ClapPlugin`/`Vst3Plugin`
code), and pushes output. Because host and child are decoupled, the
output the host reads corresponds to the input from **one block earlier**.

**This adds exactly one block of latency** — 128 frames, 2.67 ms at
48 kHz — and nothing else. It is deterministic and documented (§A.6). In
exchange the host callback is pure lock-free ring traffic: no spin, no
syscall, no unbounded wait, ever. That trade is the core of the design.
"One block of pipeline latency" is one of exactly two RT-safe ways to
consume a cross-process result without waiting in the callback (the
other is bounded-spin-to-deadline); an unbounded wait in the audio
callback is the cardinal sin the RT canon names outright (§R.5 — Bencina,
Doumler). This design is deliberately *more* conservative than the
closest prior art: yabridge blocks its audio thread on the child with no
timeout (crash-safe, not hang-safe), and Carla blocks on a *timed*
semaphore wait (hang-safe, but the callback still stalls up to the
timeout) — ours never blocks at all (§R.1).

Why decoupled-pipeline rather than a synchronous rendezvous:

- **A synchronous "wake child, wait for output this block" design cannot
  satisfy the rule.** To get the child's output in the *same* block, the
  host must wait for the child to finish. Any bounded wait is a spin
  (burns a core, and the doctrine forbids kernel waits on the audio
  thread anyway); any unbounded wait can stall the whole device callback
  behind a hung plugin — the exact failure the bridge exists to prevent.
  A bounded spin followed by silence *is* viable and gives zero added
  latency, but it burns a core per bridged plugin and is fragile under
  scheduler jitter. It is recorded as a possible future opt-in
  "low-latency bridge" mode (§E), not the default.
- **The pipeline degrades gracefully.** A late child costs one block of
  silence, then recovers on the next block it completes. A hung/dead
  child costs continuous silence until bypass trips (§C) — never a stall.

### A.3 Waking the child without a syscall on the hot path

During continuous playback the child's RT thread is **always busy** (a
new input block arrives every callback), so it spins on the input ring's
write index and picks up work within nanoseconds — the host issues **no
wake at all** on the hot path. The child only *parks* (blocks on a futex
/ semaphore) when the stream goes idle, and the host wakes it with a
single `sem_post`/`FUTEX_WAKE` **only on the idle→active transition** —
off the RT hot path, on the session thread. So:

- Host audio thread: never calls into the kernel. (Confirmed feasible in
  §A.5.)
- Child RT thread: may block — it is not the host's device callback, so
  a bounded park while idle is legal and saves the core when nothing
  plays. An adaptive spin-then-park keeps it hot across back-to-back
  blocks and only parks after an idle window.

Denormals: the child's RT thread sets FTZ/DAZ like every DSP thread
(doctrine line 18).

### A.4 Shared-memory layout (per bridged instance)

One POSIX shared-memory segment per instance (`shm_open`+`mmap` **with
`MAP_LOCKED`** on Linux; `CreateFileMapping`+`MapViewOfFile`+`VirtualLock`
on Windows), name passed to the child at spawn. The segment is mlocked so
the RT path never page-faults on it — the same precaution yabridge and
Carla take on their audio shm (§R.1). All fields are POD, fixed-size,
offset-addressed — no pointers cross the boundary. Indices are
`std::atomic<uint32_t>`; the control words are
`std::atomic<uint32_t/uint64_t>`. Layout mirrors the stage ABI's
version-first shape:

```
ControlBlock (cache-line aligned):
  atomic<u32> abi_version        // checked FIRST by both ends (stage-ABI rule)
  u32         block_frames       // = kGraphBlockFrames; refuse on mismatch
  u32         sample_rate, in_channels, out_channels
  atomic<u64> child_heartbeat    // child bumps each processed block (liveness)
  atomic<u32> child_parked       // 1 while child is futex-waiting (idle)
  atomic<u32> control_epoch      // reset/restart generation
  // host->child ring:
  atomic<u32> in_write, in_read  // SPSC indices (rt/spsc_queue.h discipline)
  InSlot      in_slots[N]        // N = 4
  // child->host ring:
  atomic<u32> out_write, out_read
  OutSlot     out_slots[N]

InSlot:
  u64   seq
  u32   frames
  u8    reset                    // fold plugin_reset() into the block
  Transport transport            // playhead, tempo, playing (VST3/CLAP want it)
  float audio_in[128*2]          // planar or interleaved; child converts once
  u16   note_count;  NoteEvent notes[kMax]      // {frame, on/off, key, velocity}
  u16   param_count; ParamChange params[kMax]   // {param_index, value01}

OutSlot:
  u64   seq
  u32   frames
  float audio_out[128*2]
  // plugin->host param gestures: discarded in v1, matching in-process
  // (clap_host.cpp:161). Reserved space left for a later version.
```

Sizing: 4 slots each way × ~2 KB ≈ 16 KB + control ≈ one page or two per
instance — negligible. `N = 4` gives the child slack to be a block or two
behind without the host dropping input.

Marshalling maps straight onto the existing staging: the host binding
fills `notes[]`/`params[]` from the same `plugin_note_on`/`_off` and
`plugin_set_param_cv` calls the runner already makes
(`graph_runner.cpp:388-422`); the child unpacks them into the CLAP/VST3
event lists the in-process host already builds
(`clap_host.cpp:394-459`, `461-546`).

**Untrusted-child hygiene:** the child is isolated plugin code. The host
treats every index/count the child writes as hostile: it bounds-checks
`out_write - out_read <= N` and clamps counts before reading a slot. A
corrupt child index can cause a dropped block, never an out-of-bounds
read in the host. (Single-process SPSC references assume a cooperative
peer; a cross-process ring must not — §R.5.)

### A.5 Deadline-miss policy (the failure behaviour)

- **Child late this block** (output ring empty): host writes silence for
  this node's output and returns. This reuses the exact path the runner
  already takes when a plugin node has no binding
  (`graph_runner.cpp:375-381`). One block of silence, no stall.
- **Child hung** (input ring fills): host push returns false and the
  block is dropped. Bounded, non-blocking; the node is already emitting
  silence via the empty output ring.
- **Child dead**: indistinguishable from "hung" to the audio thread, and
  handled identically (silence). Actual death is confirmed off-thread by
  the reaper (§B), which trips bypass (§C). The audio thread never needs
  to know *why* output stopped — only that it did, which it sees as an
  empty ring.

The audio thread therefore has a single, total rule: *fresh output →
play it; no fresh output → silence*. There is no child state that makes
it block.

**Feasibility probe (throwaway, `scratchpad/shm_atomic_probe.cpp`, not
in `src/`).** Settled the one assumption the whole design rests on: that
`std::atomic` indices in shared memory are lock-free and cross-process
coherent. Result on this Linux/x86-64 host:

- `std::atomic<uint32_t>` and `<uint64_t>` report
  `is_always_lock_free == true`.
- A `fork`ed child applying a 0.5 gain through the shm ring was observed
  coherently by the parent (parent read back `0.5`), confirming the
  release/acquire handshake straddles the process boundary.
- The parent's per-callback push+pop path measured ~9 ns with **no
  syscalls**, and when the child had no output ready the parent counted
  an underrun and moved on — it never blocked.

The probe models the §A.2 pipeline directly; it is the RT-safety argument
made executable. It builds standalone (`g++ -O2 -std=c++20 -pthread`) and
is kept only as a design artifact.

### A.6 Latency, stated honestly

Bridging a plugin adds **one graph block** of latency (2.67 ms at 48 kHz
/ 128 frames) versus hosting it in-process. The graph does not currently
do plugin delay compensation (in-process plugins are zero-latency), so
this latency is **not compensated** against dry/parallel paths. For a
serial instrument or insert effect it is inaudible transport latency; for
a plugin summed in parallel with a dry copy it is a 2.67 ms comb offset.
This is the documented cost of isolation and a reason bridging stays
opt-in. Generalised PDC is out of scope and noted in §G.

---

## B. Process lifecycle

### B.1 The child executable

A new build target — working name `nanotracker-bridge-host` — that links
the **already-factored hosting sources unchanged**: `src/ext/clap_host.cpp`,
`vst3_host.cpp`, `editor_window.cpp`, `vst3_editor_window.cpp`,
`vst3_run_loop.cpp`, `editor_host_surface_<platform>.cpp`,
`platform/shared_library.cpp`, `platform/paths.cpp`, `rt/*`, plus new
`src/ext/bridge/bridge_host_main.cpp`. It does **not** link the engine,
UI, device, or IO. In CMake it is a sibling of the `nanotracker`
executable and pulls the same `clap` / `vst3_hosting` targets
(`CMakeLists.txt:246-253`, `273-295`). The host locates the child binary
next to its own executable using the existing `executable_dir` search
(`platform/paths.cpp:29-42`).

### B.2 Spawn + control channel

Host spawns the child with `posix_spawn` (Linux) / `CreateProcess`
(Windows), passing on the command line: the shm segment name, plugin kind
(clap/vst3), library path, plugin id, and sample rate. Alongside the shm
segment, a **Unix-domain socketpair** (inherited fd; a named pipe pair on
Windows) carries the **control channel** — everything that is main-thread
or too large for the RT ring: the version handshake, plugin state
save/load blobs, editor open/close/resize, and shutdown. The socket is
reliable, ordered, and — critically — returns **EOF when the child dies**,
which is one of the three crash signals (§B.4).

### B.3 Handshake / version negotiation (stage-ABI discipline)

First bytes on the control channel, both directions:
`{magic, abi_version, block_frames, sizeof(ControlBlock)}`. The host
checks `abi_version` **before reading any other field** — the exact rule
`ntp_stage_abi.h:38,138` states — and refuses a mismatch with a message
naming both versions. The shm `ControlBlock.abi_version` is re-checked by
the child before it trusts any other shm field. Host and child ship
together and version in lockstep, so this is an *internal* protocol
(GPLv3, lives in `src/ext/bridge/bridge_protocol.h`), not a public ABI
like `ntp_stage_abi.h` — but it borrows the same version-first, additive-
growth, reserved-field discipline so a future protocol bump is clean.

### B.4 Crash detection — three signals, none on the audio thread

The detection is deliberately split so the audio thread never waits on a
process primitive:

1. **Heartbeat staleness (audio-thread-observable, no syscall).** The
   child bumps `child_heartbeat` every processed block. The audio thread
   reads it lock-free; if output has been absent and the heartbeat has
   not advanced for K blocks, the binding flags itself *not producing*
   and keeps emitting silence. This is an *observation*, not a
   confirmation — the audio thread never calls `waitpid`.
2. **Session-thread reaper (confirmation).** A non-RT monitor confirms
   actual death via `pidfd_open` + poll (Linux) / `WaitForSingleObject`
   (Windows), or `waitpid(WNOHANG)`. `pidfd` is preferred: a pollable fd,
   no process-global `SIGCHLD` handler to coordinate with the rest of the
   app. On confirmed death it transitions the node to *bypassed*, raises
   the badge, and arms restart (§C).
3. **Control-channel EOF.** The reaper also `poll()`s the control socket;
   the child's death closes it, giving a second independent confirmation
   and covering a child that dies between heartbeat ticks.

Division of labour, exactly as the brief frames it: **the audio thread
sees "no fresh output" and bypasses instantly (sound continues); the
reaper confirms death and drives the badge + restart.**

### B.5 Graceful shutdown

Host sends `shutdown` on the control channel; child deactivates+destroys
the plugin (`ClapPlugin`'s destructor already does
deactivate→destroy, `clap_host.cpp:346-353`), unmaps shm, and exits 0.
Host reaps with a bounded wait, then `SIGKILL`/`TerminateProcess` any
child that overstays. On host exit every child gets the same sequence.

---

## C. Crash → bypass → restart

### C.1 Bypass semantics (which: silence or passthrough)

- **Instrument (no audio input):** bypass = **silence**. There is no
  input to pass; this matches the existing no-binding path
  (`graph_runner.cpp:375-381`).
- **Effect (has audio input):** bypass = **pass the dry input through**,
  not silence. A crashed reverb should not punch a hole in the track; the
  dry signal is the least-destructive fallback. The binding knows its own
  `has_audio_input()` at construction (`clap_host.h:106`,
  `vst3_host.h:95`), so it chooses correctly without touching the graph.

This is a deliberate, small improvement over the in-process crash path,
which fills silence on `CLAP_PROCESS_ERROR` (`clap_host.cpp:538-540`). It
is new host behaviour, not a web divergence, so it belongs in this design
rather than `FIXES.md`. Bypass is a wait-free branch on an atomic state
word the binding owns; flipping it costs nothing on the audio thread.
This auto-deactivate-and-keep-running posture is the same one Carla and
Bitwig ship (§R.1, §R.2).

### C.2 Node badge

The binding exposes an atomic state enum — `ok`, `late`, `crashed` — that
the UI reads through the snapshot/poll path (never the RT path, same
discipline as every other audio→UI readout). The workspace node draws a
**loud red "CRASHED — click to restart" badge** in the `crashed` state,
and a subtler "late/underrunning" hint in `late`. The `late` state also
gives users a truthful signal that a plugin is too heavy for the pipeline.

### C.3 One-click restart — the binding outlives the child

The key structural choice: the **`BridgedPlugin` binding object is
stable**; only the child process behind it is replaced. The graph binds
the binding pointer once (`project_session.cpp:1356-1376` via
`GraphRunner::bind_plugin`), and restart swaps the *child*, not the
binding — so **restart touches neither the graph, the bundle, nor the
reclamation fence.** The audio thread keeps calling the same binding,
which flips from *crashed* (bypass) back to *producing* once the fresh
child is up. Sequence:

1. Reaper has confirmed death and set state = `crashed`.
2. On the user's click (session thread): respawn the child (§B.2),
   re-handshake (§B.3), recreate the plugin instance in the child.
3. Push the **last-known-good state blob** to the fresh child over the
   control channel (child calls `load_state`, reusing
   `clap_host.cpp:593` / `vst3_host.cpp:105`).
4. Clear the control epoch, reset ring indices, set state = `ok`. Audio
   resumes on the next block the child completes.

### C.4 Where the restart state comes from

After a crash the child is gone and cannot `save_state`, so the host must
already hold a recent snapshot. The `BridgedPlugin` keeps a **shadow copy
of the last-known-good state**, refreshed opportunistically on the main
thread while the child is alive: at load, after every editor session
closes, and on an idle timer when the instance is dirty. This is exactly
the blob `assemble_write_extras` already pulls for persistence
(`project_session.cpp:306,321`) — the shadow copy *is* what a save would
write. Worst case a crash loses a few seconds of un-snapshotted tweaks;
that is acceptable and documented (Carla ships the same limitation, warned
in its crash dialog, §R.1). A session save (Ctrl-S) always
captures the current shadow, so **the session stays saveable with a dead
child** — the XPLG writer reads the shadow blob and the parameter
snapshot, never the child (`project_session.cpp:298-326`).

### C.5 Exit criterion

A deliberately-crashing **fixture plugin** (a tiny CLAP that segfaults on
the Nth `process()` call — lives under `tests/fixtures/`, headless). Its
death must leave: (a) the device callback still running and every other
node audible, (b) the crashed node bypassed (dry for effects, silent for
instruments), (c) the session saveable to FTRK and reloadable, (d)
one-click restart restoring the plugin to its shadow state. This is the
stage's headline exit criterion (00-plan.md line 76).

---

## D. Editor embedding cross-process

### D.1 Where the editor lives

The plugin editor is created **in the child**, by the *unchanged*
`ClapEditorWindow` / `Vst3EditorWindow` code
(`editor_window.cpp:37-81`, `vst3_editor_window.cpp:96-139`). That code
already opens a bare top-level OS window (`EditorHostSurface`,
`editor_host_surface.h`) and has the plugin embed itself into it — CLAP
via `gui->set_parent` (`editor_window.cpp:68-69`), VST3 via
`view->attached(handle, kPlatformTypeX11EmbedWindowID)`
(`vst3_editor_window.cpp:130`). The child runs this on its **main/UI
thread** (distinct from its RT processing thread), preserving the
CLAP main/audio split the host already encodes
(`clap_host.cpp:42-57`, the `t_is_main_thread` flag).

### D.2 Reparenting the child's window into the host (X11, v1)

The child creates its `EditorHostSurface` window and sends its native
handle (X11 `Window` id) to the host over the control channel. The host
reparents it under a host-owned container:

- **X11:** `XReparentWindow(display, child_window, host_container, 0, 0)`.
  Window ids are display-global, so this works across processes — it is
  the same XEmbed-family mechanism plugin hosts and yabridge use. The
  host frames it with the node title and a close button, and relays
  container resizes to the child (which answers via the existing
  `set_size`/`take_resize` renegotiation, `editor_window.cpp:104-118`).
- **Foreign-window safety:** because the embedded window belongs to
  another process, the host must handle `DestroyNotify`/`UnmapNotify` for
  it gracefully — a child crash while the editor is open must tear the
  container down without corrupting the host's own window tree. The
  existing X11 pump already selects `StructureNotifyMask`
  (`editor_host_surface_x11.cpp:48`); it gains reparent/destroy handling.

### D.3 Event pump across processes

The plugin's fd/timer callbacks (CLAP `posix-fd`/`timer`, VST3
`IRunLoop`) **must run in the child, on the child's UI thread** — that is
where the editor lives. The child runs the existing per-frame pump
(`ClapEditorWindow::update` → `pump_main_thread`, `clap_host.cpp:644`;
`Vst3RunLoop::dispatch`, `vst3_run_loop.cpp`). The host does **not** pump
the plugin's loop; it only relays a few control messages (open, close,
resize) and hosts the container window. Input events reach the plugin
naturally: X11 delivers them to the reparented child window, whose owner
(the child) handles them. The IRunLoop threading trap from prior research
(callbacks must be on the UI thread, never a worker
— [../Plan_PostV1/Research/07-plugin-gui-hosting.md](../Plan_PostV1/Research/07-plugin-gui-hosting.md)
line 34) is satisfied for free: the child's pump is its UI thread.

### D.4 Windows

Win32 `SetParent(child_hwnd, host_hwnd)` also works cross-process, but
cross-process parenting has focus/activation and input-routing quirks,
and the VST3 SDK is effectively MSVC-only on Windows
([../Plan_PostV1/Research/07-plugin-gui-hosting.md](../Plan_PostV1/Research/07-plugin-gui-hosting.md)
line 46). Cross-process editor embedding on Windows is **deferred** with
the rest of the Windows bridge (§E).

---

## E. Opt-in surface & scope

### E.1 Per-plugin toggle, default off

Per-plugin opt-in (not a global always-on sandbox) is a deliberate call,
backed by the field: Bitwig and REAPER both expose isolation as a
per-plugin/per-scope *choice* (§R.2), and Ardour's context-switch math is
the reason it stays a choice — isolation is not free, so it is spent only
where a plugin is actually flaky (§R.3). A **"Bridge (isolate in a
separate process)"** control, default **off**, in two places:

- **Settings:** a global default (off) plus a per-plugin remembered
  choice, persisted with the rest of settings (`src/io/settings.cpp`).
- **Workspace node:** a toggle on the plugin node's inspector/context
  menu, so isolation is chosen per instance where the risk is.

When set, the session builds a `BridgedPlugin` binding instead of an
in-process `ClapPlugin`/`Vst3Plugin` at `add_clap_node`/`add_vst3_node`
time (`project_session.cpp:1129-1186`) and at XPLG load
(`project_session.cpp:228-257`). Persistence: one additive `bridged` bool
on the XPLG per-instance record ([../ftrk-format.md](../ftrk-format.md)
line 124 / `io/ftrk_reader.h:66-70`); the reader already tolerates files
without newer fields (format back-compat rule, 00-plan Stage 25 note),
so old files load unbridged.

### E.2 v1 scope — honest platform calls

- **Linux-first, complete.** Full bridge on Linux: shm pipeline +
  crash/bypass/restart + X11 reparented editor. This matches the port's
  Linux-primary posture (the run loop and X11 surface are the mature
  paths).
- **Both CLAP and VST3** bridge through the same child (it links both
  hosts). VST3-in-child costs a little more (COM lifetime, `IRunLoop` in
  the child) but reuses `vst3_host.cpp` wholesale.
- **Windows: deferred.** The shm pipeline and crash/restart are portable
  (Win32 `CreateFileMapping`/`CreateProcess` equivalents, and
  `shared_library.cpp`/`paths.cpp` are already dual-platform), but
  **cross-process editor embedding on Win32 is deferred** (§D.4). Since
  the Windows beta opens the next cycle (per project memory), the
  cleanest call is: **v1 ships the bridge on Linux; the Windows bridge is
  a fast-follow.** Flagged for the owner (§G).
- **One child per bridged instance.** Simpler crash isolation and
  lifecycle than a shared multi-plugin sandbox process; the latter (to
  amortise per-child memory) is a deferred optimisation (§G).
- **Deferred behaviours, matching in-process limits:** plugin→host param
  gestures stay discarded (`clap_host.cpp:161`); sub-block MIDI timing
  stays block-head only (`graph_runner.cpp:386`). No regressions, no new
  promises.

---

## F. Staged implementation plan

Five sub-stages, each landing with green CI. Exit criteria are concrete
and testable; the ritual (verify → ledgers → tar) is unchanged.

### S29a — IPC ring + echo child  ⚠ RISKIEST
The shared-memory `ControlBlock` + SPSC block rings
(`src/ext/bridge/shm_block_ring.h`, `bridge_protocol.h`), spawn + control
socket + version handshake, a trivial **echo child** (copies input→output,
bumps heartbeat), and the host-side
`BridgedPlugin : GraphPluginBinding` implementing the one-block pipeline.
**Exit:** an echo child round-trips audio at exactly one block latency;
`process_block` is allocation-free under the debug allocator
(`rt_assert.cpp`) with an `RtScope` asserted; a latency test measures one
block; a "kill the child mid-stream" test yields silence, never a stall;
the shm atomics are verified lock-free on CI hosts.

### S29b — real CLAP in the child
Child instantiates a real `ClapPlugin` driven by the ring; notes, CV
params, and transport marshalled through the slots; state save/load over
the control channel. **Exit:** a real CLAP synth bridged produces audio
matching the same plugin in-process within float tolerance (accounting
for the one-block offset); state round-trips through the control channel.

### S29c — crash → bypass → restart
Heartbeat + reaper (`pidfd`/control-EOF/`waitpid`), audio-thread bypass
(dry-passthrough for effects, silence for instruments), the node badge
state enum, shadow-state capture, and one-click restart. The
deliberately-crashing fixture plugin. **Exit:** the §C.5 criterion —
crash leaves audio running and the session saveable; restart restores the
shadow state.

### S29d — editor embedding cross-process (X11)
Child creates the editor; host reparents the child window into a
container; resize/close relayed; child runs the fd/timer/IRunLoop pump.
**Exit:** a bridged plugin's editor opens embedded, takes input, resizes,
and closes cleanly; a crash while the editor is open tears down without
corrupting the host window tree (a read-only X-tree check like the
existing editor verification, ../Plan_PostV1/09-plugin-platform.md line
61).

### S29e — opt-in UI + VST3-in-child + polish
The settings + node-menu toggle, XPLG `bridged` persistence, VST3 through
the same child, auto-param-panel fallback wiring, and doc/ledger updates
(`ftrk-format.md`, `DEPENDENCIES.md`). **Exit:** toggling a plugin
bridged/unbridged survives save→load; VST3 bridges; CI green.

**Riskiest: S29a.** It carries the novel, RT-critical, cross-process
work — the shm ring, the deadline policy, and cross-process atomics. If
its RT-safety is not airtight, nothing downstream is safe. It is
front-loaded deliberately, and the §A.5 probe already de-risks its central
assumption before a line of `src/` code is written. Second-riskiest is
S29d (foreign-window lifetime on a crash).

---

## G. Risks & open questions

Design-verification note: the RT argument in §A is airtight in the sense
the brief requires — **there is no child state (fresh / slow / hung /
dead) under which the host audio callback blocks unboundedly**; the worst
case is one block of silence via an empty ring (§A.5), reusing the
runner's existing no-binding silence path. The crash path is provably
non-fatal because detection is split off the audio thread (§B.4) and the
binding outlives the child (§C.3). The open questions below are product
and tuning calls, not holes in that argument.

1. **One-block added latency, uncompensated (§A.6).** Ship as documented
   latency, or invest in plugin delay compensation for bridged nodes?
   Recommendation: document it; PDC is a separate, larger feature.
   **Owner call.**
2. **Windows bridge scope (§E.2).** Linux-only in v1 with Windows as a
   fast-follow, given the Windows beta opens the next cycle?
   Recommendation: yes. **Owner call.**
3. **Child CPU while active (§A.3).** One spinning RT thread per bridged
   plugin burns a core during playback (adaptive spin-then-park mitigates
   idle cost). Acceptable for the expected count (typically 1–3 bridged
   plugins)? Confirm the model.
4. **Child RT scheduling.** Should the child request `SCHED_FIFO` for its
   processing thread, and should the host *propagate* its own audio-thread
   priority to the child (yabridge does exactly this, §R.1)? Needs the
   same privileges as the main audio thread; if unavailable the child
   runs at normal priority and leans on the one-block buffer to absorb
   jitter. Config/owner call.
5. **Shadow-state cadence (§C.4).** How often to snapshot for crash
   recovery — freshness vs overhead. Start conservative (load + editor
   close + idle-dirty timer); tune if users report lost tweaks.
6. **Multi-plugin sandbox (§E.2).** One child per instance in v1; a
   shared sandbox process to amortise memory is a deferred optimisation.
7. **Not a security sandbox.** This is *crash* isolation, not a
   privilege sandbox — the child runs plugin code with the host user's
   permissions. Isolating a *malicious* plugin (seccomp/namespaces) is
   explicitly out of scope. The one hardening we do adopt is treating the
   child's shm indices/counts as untrusted (§A.4).

---

## Research — current art (independent design; licences noted)

The bridge is designed independently from the sources below. Where a
source is copyleft its architecture informs the design but no code is
copied; NanoTracker is GPLv3, so a GPLv3 dependency would be compatible,
but the shm ring is our own `rt/spsc_queue.h` shape lifted into shared
memory. Licences are called out because they bound what may be borrowed.

### R.1 The two closest analogs, and how this design differs from both

- **yabridge** (Robbert van der Helm, **GPLv3**) — bridges Windows
  plugins into Linux hosts under Wine, but its host↔child audio path is
  the archetype. Architecture doc:
  <https://github.com/robbert-vdh/yabridge/blob/master/docs/architecture.md>.
  It uses **one mlocked POSIX shared-memory block** (`MAP_LOCKED`),
  offset-partitioned per bus/channel and *reused every block* — **not a
  ring** (`src/common/audio-shm.h`,
  <https://github.com/robbert-vdh/yabridge/blob/master/src/common/audio-shm.h>)
  — with a **blocking `AF_UNIX` socket round-trip as the doorbell**: the
  audio thread copies input into shm, sends a small request, then
  **blocks on the reply `Ack`** with *no timeout* (`do_process` in
  `src/plugin/bridges/vst2.cpp`,
  <https://github.com/robbert-vdh/yabridge/blob/master/src/plugin/bridges/vst2.cpp>).
  Two lessons lifted: **mlock the audio shm** so the RT path never
  page-faults, and **propagate the host's RT scheduling priority to the
  child's worker thread**. One lesson deliberately *rejected*: yabridge's
  synchronous, un-timed socket wait means a **hung** plugin stalls the
  DAW's audio thread — it buys crash *containment*, not hang-safety. Our
  §A pipeline never waits, so it survives hangs too. yabridge also
  defaults to **one child process per plugin** (its "group" mode shares a
  process and, in its own words, drops isolation between the grouped
  plugins) — the same per-instance default we take (§E).

- **Carla** (falkTX, **GPL-2.0-or-later**) — the most complete open OOP
  host. `CarlaPluginBridge`/`CarlaEngineBridge` use **four shm segments**
  — one flat audio pool + three lock-free SPSC control rings
  (`CarlaRingBuffer.hpp`,
  <https://github.com/falkTX/Carla/blob/main/source/utils/CarlaRingBuffer.hpp>)
  — synchronised by a **paired cross-process semaphore** (a **futex** on
  Linux; `CarlaSemUtils.hpp`,
  <https://github.com/falkTX/Carla/blob/main/source/utils/CarlaSemUtils.hpp>).
  Crucially, the host's per-block wait is **timed** (`waitForClient`
  /`carla_sem_timedwait`): on timeout it sets `fTimedOut`,
  auto-deactivates (bypasses) the plugin, and raises a user-facing *"Plugin
  '…' has crashed! Saving now will lose its current settings."*
  (`CarlaPluginBridge.cpp`,
  <https://github.com/falkTX/Carla/blob/main/source/backend/plugin/CarlaPluginBridge.cpp>);
  a 30 s child-side ping watchdog reaps orphans; the protocol carries an
  explicit version (`CarlaBridgeDefines.hpp`). This is the crash→bypass
  UX §C adopts almost verbatim (including the honest "a save right after
  a crash loses the child's un-snapshotted state" warning). We diverge on
  one point: Carla's audio thread still *blocks* on a timed wait each
  block; ours never blocks (one-block pipeline), trading Carla's
  zero-latency-but-can-stall-to-the-timeout for our
  one-block-latency-but-never-stalls. All Carla bridge code is GPL —
  design reference only.

### R.2 Productised isolation UX (policy references)

- **Bitwig Studio** — shipped per-plugin sandboxing in 2.5 (2019) with
  **selectable granularity**: *Within Bitwig* / *Together* / *By
  Manufacturer* / *By Plug-in* / *Individually*, trading RAM/CPU against
  blast radius; a crash is discreet and audio keeps playing.
  <https://www.bitwig.com/learnings/plug-in-hosting-crash-protection-in-bitwig-studio-20/>.
  The granularity axis is why §E keeps one-child-per-instance in v1 and
  records a shared sandbox as a deferred *policy*, not a missing feature.
- **REAPER** — per-plugin "Run as → Separate/Dedicated process," a direct
  precedent for the per-plugin opt-in toggle (§E):
  <https://forum.cockos.com/showthread.php?t=97131>.
- **Tracktion Waveform** — a JUCE-based DAW with per-plugin OOP
  sandboxing; its bug reports flag the hard parts we design around
  (cross-boundary GUI-vs-audio param access; orphaned-sandbox cleanup on
  Linux): <https://forum.juce.com/t/possible-tracktion-waveform-11-5-sandbox-bug-with-non-automatable-audio-parameters-and-vst3/44878>.

### R.3 The reality check (why opt-in, not always-on)

- **Ardour** deliberately keeps plugins in-process and shows the math:
  a context switch costs ~3–300 µs, and a large session can need
  hundreds of switches per audio block — tens of milliseconds of
  overhead, untenable at low latency.
  <https://ardour.org/plugins-in-process.html>. A tracker's plugin counts
  are far smaller, so bridging is survivable — but this is exactly why
  bridging stays **opt-in per plugin** (§E) rather than a global default,
  and why in-process remains the norm.

### R.4 What the plugin standards do (and don't) give us

- **CLAP** (free-audio/clap, **MIT**) defines *no* OOP/sandbox extension,
  but is the easy bridge target: a pure C ABI, a **POD `clap_process`**
  with a flat sample-sorted event list
  (<https://github.com/free-audio/clap/blob/main/include/clap/process.h>),
  and an explicit *"only one OS thread is the audio-thread at a time"*
  contract (`ext/thread-check.h`,
  <https://github.com/free-audio/clap/blob/main/include/clap/ext/thread-check.h>)
  that a child process satisfies trivially. Caution: the CLAP **thread-pool**
  extension is *not* an OOP tool and its `request_exec` *"blocks until
  completion"* — the opposite of what our callback can afford
  (<https://github.com/free-audio/clap/blob/main/include/clap/ext/thread-pool.h>).
- **VST3** (Steinberg, SDK MIT since Nov 2025) also has no OOP API, and
  is harder to bridge because its interface layer is **COM-style
  `FUnknown`** (refcounting + `queryInterface` by IID) that must be
  proxied, not just called
  (<https://steinbergmedia.github.io/vst3_doc/base/funknown_8h.html>).
  The dev portal does acknowledge a host "can run each component
  (processor and controller) in a different context, even on different
  computers" — an allowance, not machinery. This is why §F bridges CLAP
  first (S29b) and VST3 later (S29e).

### R.5 RT-safety canon (the §A argument's foundations)

- **Ross Bencina, "Real-time audio programming 101: time waits for
  nothing"** — the audio thread must do nothing of unbounded time, never
  lock, and communicate with non-RT tasks only through
  "atomics, flags, dual counters, queues, ring buffers."
  <http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing>
  (mirror: <https://lwn.net/Articles/452630/>).
- **Timur Doumler, "Using locks in real-time audio processing, safely"**
  — the RT thread may `try_lock`/wake but must **never wait**; `sem_post`
  is a single bounded op and `std::atomic::notify_one` is *not*
  guaranteed RT-safe. <https://timur.audio/using-locks-in-real-time-audio-processing-safely>.
- **Fabian Renn-Giles & Dave Rowland, "Real-Time 101" (ADC'19)** — the
  canonical lock-free-audio toolbox talk.
  <https://www.youtube.com/watch?v=Q0vrQFyAdWI>.
- **Jeff Preshing, "Acquire and Release Semantics"** — the memory-order
  discipline the shm ring uses.
  <https://preshing.com/20120913/acquire-and-release-semantics/>; and
  "Semaphores are Surprisingly Versatile" for the spin-then-block child
  wake (§A.3): <https://preshing.com/20150316/semaphores-are-surprisingly-versatile/>.
- **Cross-process wakeup primitives (Linux):** `futex(2)` works across
  processes when `FUTEX_PRIVATE_FLAG` is omitted, and PI-futexes exist
  for the inversion the *wait* side risks (we only ever wake, so we
  don't): <https://man7.org/linux/man-pages/man2/futex.2.html>. `sem_post`
  is officially **async-signal-safe** — the strongest formal "bounded,
  reentrant" guarantee for an RT wake:
  <https://man7.org/linux/man-pages/man3/sem_post.3p.html>,
  <https://man7.org/linux/man-pages/man7/signal-safety.7.html>. Windows
  equivalents for the future port: `CreateFileMapping`/`MapViewOfFile`
  (<https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory>)
  and `CreateSemaphore`/`ReleaseSemaphore` or `SetEvent`
  (<https://learn.microsoft.com/en-us/windows/win32/sync/semaphore-objects>).

### R.6 The one lawfully-liftable transport reference

- **`reales/discoLink`** (**MIT**) — a small C++17 lock-free SPSC
  shared-memory ring transport with `shm_open`/`CreateFileMapping`, a
  zero-alloc RT path, and **zero-fill on disconnect** (exactly the §A.5
  silence-on-empty policy). <https://github.com/reales/discolink>. The
  only referenced codebase whose licence permits borrowing transport code
  rather than re-implementing — worth reading alongside Carla's GPL
  design if S29a wants a head start.

### R.7 Prior in-tree research this builds on

The v1 editor-window work already pinned the Linux GUI-hosting facts this
bridge reuses cross-process: CLAP `set_parent` / VST3 `IPlugView::attached`
embedding, the host-supplied `IRunLoop`, and the **UI-thread-only**
callback rule for JUCE-built plugins
([../Plan_PostV1/Research/07-plugin-gui-hosting.md](../Plan_PostV1/Research/07-plugin-gui-hosting.md);
[../Plan_NativePort/Research/05-clap-hosting.md](../Plan_NativePort/Research/05-clap-hosting.md)).

## H. Locked decisions (owner, 2026-07-19)

1. **One-block latency ships documented; no PDC.** The ~2.67ms
   (128f @ 48k) pipeline latency is surfaced on the per-plugin opt-in
   toggle and in the docs. Plugin-delay-compensation is a separate
   graph-wide feature (nothing has it today) and does not gate the
   bridge. In-process plugins are unaffected.
2. **Linux-first, Windows fast-follow.** v1 is the complete Linux
   bridge — shm IPC + crash/bypass/restart + X11 cross-process
   reparented editor, CLAP and VST3. The shm/crash core is written
   portable; Win32 `SetParent` editor embedding lands in the
   Windows-beta follow-up cycle, not v1.
3. **Best-effort RT scheduling, no privileges required.** The child
   RT thread requests `SCHED_FIFO` (propagating the host audio
   priority) and falls back silently to normal scheduling when the
   rtprio limit denies it — the standard Linux-audio posture. The
   feature works out of the box; raising rtprio limits (rtkit /
   limits.conf) is a documented power-user tuning, never a
   precondition. Not a security sandbox: the child runs plugin code
   with the user's own permissions; seccomp/namespaces are out of
   scope (crash isolation only).
