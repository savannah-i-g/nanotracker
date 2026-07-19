// ext/bridge/bridge_protocol — the host<->child shared-memory wire
// protocol for the out-of-process plugin bridge (Stage 29).
//
// This header is the single source of truth for the shared-memory
// layout AND for the wait-free ring operations that straddle the
// process boundary. Both the host binding (bridged_plugin.cpp) and the
// child executable (bridge_host_main.cpp) include it, so the memory
// ordering below is written and reviewed once, in one place.
//
// ── The load-bearing rule (design 01-plugin-bridge-design.md §A.1) ────
// The host audio thread performs only *wait-free* ring operations on
// this segment: a bounded number of atomic loads/stores and a bounded
// memcpy, never a syscall, lock, or unbounded wait. Its worst case
// under any child state (slow / hung / dead) is one block of output
// silence via an empty output ring (§A.5). Everything here serves that
// rule; nothing here may block.
//
// ── Two SPSC rings, decoupled by one block (§A.2) ─────────────────────
// host -> child input ring  (producer = host audio thread, consumer =
//                            child RT thread): audio + events per block.
// child -> host output ring (producer = child RT thread, consumer =
//                            host audio thread): processed audio.
// The two ends do not rendezvous per block; the output the host reads
// corresponds to the input from one block earlier. That single block of
// pipeline latency is the deliberate, documented cost of never waiting
// (§A.6, owner decision §H.1).
//
// ── Version-first, additive-growth (§A.4, §B.3) ───────────────────────
// abi_version is the FIRST field and is checked before any other field
// is read, mirroring the native-stage ABI discipline
// (include/ntp/ntp_stage_abi.h:38,138). Host and child ship together
// and version in lockstep, so this is an internal protocol, not a
// public ABI — but it borrows the same version-first, fixed-offset,
// POD-only shape so a future bump stays clean. No pointers ever cross
// the boundary; every field is fixed-size and offset-addressed.
//
// ── Untrusted child (§A.4) ────────────────────────────────────────────
// The child hosts isolated plugin code and is treated as hostile. The
// host's output-ring consumer bounds-checks the child-written write
// index before dereferencing a slot and clamps the child-written frame
// count, so a corrupt child index can only drop a block, never cause an
// out-of-bounds read in the host.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace nt::ext::bridge {

// Segment identity + protocol version. abi_version bumps whenever a
// change could make an older peer misread the layout; block_frames /
// struct_size are cross-checked in the handshake so a stale segment is
// refused rather than misinterpreted.
inline constexpr std::uint32_t kBridgeMagic = 0x4E544252U; // "NTBR"
inline constexpr std::uint32_t kBridgeAbiVersion = 1U;

// Fixed block geometry. kBridgeBlockFrames must equal
// audio::kGraphBlockFrames — asserted where both headers are visible
// (bridged_plugin.cpp); kept literal here so the child never pulls in
// the audio engine.
inline constexpr std::uint32_t kBridgeBlockFrames = 128U;
inline constexpr std::uint32_t kBridgeChannels = 2U; // interleaved stereo

// Interleaved samples in one full block (size_t so array extents below
// do not widen a uint32_t product).
inline constexpr std::size_t kBridgeBlockSamples =
    static_cast<std::size_t>(kBridgeBlockFrames) * kBridgeChannels;

// Ring depth (power of two). N = 4 gives the child slack to fall a
// block or two behind without the host dropping input (§A.4).
inline constexpr std::uint32_t kBridgeRingSlots = 4U;
static_assert((kBridgeRingSlots & (kBridgeRingSlots - 1U)) == 0U,
              "ring depth must be a power of two for index masking");

// Per-block event capacity. Carried by the layout so the ring is
// S29b-ready; the S29a echo child leaves the counts at zero and ignores
// the arrays (real note/CV/transport marshalling lands in S29b).
inline constexpr std::uint32_t kBridgeMaxNotesPerBlock = 32U;
inline constexpr std::uint32_t kBridgeMaxParamsPerBlock = 32U;

// Child idle-park tuning (§A.3): the child spins on the input ring and
// only parks (futex-waits) after this idle window, which must exceed one
// block period (2.67 ms @ 48k) so it never parks during continuous
// playback. The park wait itself times out to re-poll the shutdown flag.
inline constexpr std::int64_t kBridgeParkAfterMs = 8;
inline constexpr std::int64_t kBridgeParkTimeoutNs = 50'000'000; // 50 ms

// ── Slot payloads (POD, fixed size) ──────────────────────────────────

struct NoteEvent {
    std::uint32_t frame = 0; // sample offset within the block
    std::uint32_t on = 0;    // 1 = note-on, 0 = note-off
    std::int32_t key = 0;    // MIDI key number
    float velocity = 0.0F;   // 0..1
};

struct ParamChange {
    std::uint32_t index = 0; // position in the binding's parameter list
    float value01 = 0.0F;    // clamped 0..1, mapped by the child
};

struct Transport {
    double playhead_seconds = 0.0;
    double tempo_bpm = 0.0;
    std::uint32_t playing = 0;
    std::uint32_t reserved = 0;
};

// host -> child: one block's input. audio_in is interleaved stereo; the
// note/param arrays are valid only for the first note_count/param_count
// entries. reset folds a plugin_reset() request into the block.
struct alignas(64) InSlot {
    std::uint64_t seq = 0;
    std::uint32_t frames = 0;
    std::uint32_t reset = 0;
    Transport transport;
    std::uint32_t note_count = 0;
    std::uint32_t param_count = 0;
    std::array<NoteEvent, kBridgeMaxNotesPerBlock> notes{};
    std::array<ParamChange, kBridgeMaxParamsPerBlock> params{};
    std::array<float, kBridgeBlockSamples> audio_in{};
};

// child -> host: one block's processed audio.
struct alignas(64) OutSlot {
    std::uint64_t seq = 0;
    std::uint32_t frames = 0;
    std::uint32_t reserved = 0;
    std::array<float, kBridgeBlockSamples> audio_out{};
};

static_assert(std::is_standard_layout_v<InSlot>);
static_assert(std::is_standard_layout_v<OutSlot>);
static_assert(std::is_trivially_copyable_v<InSlot>);
static_assert(std::is_trivially_copyable_v<OutSlot>);

// ── Control block (cache-line aligned; version-first) ─────────────────
// Indices are std::atomic<uint32_t> monotonic counters, masked on
// access (never wrapped explicitly — SPSC tolerates 2^32 wrap while
// capacity < 2^32, matching rt/spsc_queue.h). Producer and consumer
// indices sit on separate cache lines to avoid false sharing, exactly
// as SpscQueue aligns head_/tail_.
struct alignas(64) ControlBlock {
    // Header — checked FIRST, in this order, before any field below.
    std::atomic<std::uint32_t> abi_version;
    std::uint32_t magic = 0;
    std::uint32_t block_frames = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t in_channels = 0;
    std::uint32_t out_channels = 0;
    std::uint32_t struct_size = 0; // sizeof(ControlBlock); handshake cross-check

    // Liveness + lifecycle (all observed lock-free; none on the audio
    // thread waits on a process primitive — §B.4).
    std::atomic<std::uint64_t> child_heartbeat; // child bumps each processed block
    std::atomic<std::uint32_t> child_ready;     // child sets 1 after validating the header
    std::atomic<std::uint32_t> child_status;    // 0 = ok; reserved for S29c reasons
    std::atomic<std::uint32_t> shutdown;        // host sets 1 to request child exit
    std::atomic<std::uint32_t> child_parked;    // 1 while the child is futex-waiting
    std::atomic<std::uint32_t> wake;            // futex word: host bumps to wake a parked child
    std::atomic<std::uint32_t> control_epoch;   // reset/restart generation (reserved, S29c)

    // host -> child ring indices (producer = host, consumer = child).
    alignas(64) std::atomic<std::uint32_t> in_write;
    alignas(64) std::atomic<std::uint32_t> in_read;
    // child -> host ring indices (producer = child, consumer = host).
    alignas(64) std::atomic<std::uint32_t> out_write;
    alignas(64) std::atomic<std::uint32_t> out_read;

    // Ring storage.
    alignas(64) std::array<InSlot, kBridgeRingSlots> in_slots{};
    std::array<OutSlot, kBridgeRingSlots> out_slots{};
};

static_assert(std::is_standard_layout_v<ControlBlock>);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "shm indices must be lock-free to be coherent across the process boundary");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "heartbeat must be lock-free to be observed wait-free by the audio thread");
static_assert(offsetof(ControlBlock, abi_version) == 0,
              "abi_version must be first: it is read before any other field (stage-ABI rule)");

// ── Wait-free SPSC ring operations (§A.1) ─────────────────────────────
// Acquire/commit (producer) and peek/release (consumer) split the
// classic value-copy SpscQueue so the producer fills the shm slot in
// place — no redundant multi-kilobyte copy through a stack temporary.
// Memory ordering matches rt/spsc_queue.h exactly:
//   producer: load own index relaxed, load peer index ACQUIRE (observe
//             the consumer's frees), publish with a RELEASE store.
//   consumer: load own index relaxed, load peer index ACQUIRE (observe
//             the producer's slot writes), free with a RELEASE store.
// The RELEASE on commit/publish pairs with the ACQUIRE on the peer's
// peek so the slot contents are fully visible before the index that
// exposes them. None of these functions allocate, lock, or wait.

inline InSlot* in_ring_acquire(ControlBlock& cb) {
    const std::uint32_t w = cb.in_write.load(std::memory_order_relaxed);
    const std::uint32_t r = cb.in_read.load(std::memory_order_acquire);
    if (w - r >= kBridgeRingSlots) {
        return nullptr; // full: child is behind — drop, never retry (§A.5)
    }
    return &cb.in_slots[w & (kBridgeRingSlots - 1U)];
}

inline void in_ring_commit(ControlBlock& cb) {
    const std::uint32_t w = cb.in_write.load(std::memory_order_relaxed);
    cb.in_write.store(w + 1U, std::memory_order_release);
}

inline const InSlot* in_ring_peek(ControlBlock& cb) {
    const std::uint32_t r = cb.in_read.load(std::memory_order_relaxed);
    const std::uint32_t w = cb.in_write.load(std::memory_order_acquire);
    if (r == w) {
        return nullptr; // empty
    }
    return &cb.in_slots[r & (kBridgeRingSlots - 1U)];
}

inline void in_ring_release(ControlBlock& cb) {
    const std::uint32_t r = cb.in_read.load(std::memory_order_relaxed);
    cb.in_read.store(r + 1U, std::memory_order_release);
}

inline OutSlot* out_ring_acquire(ControlBlock& cb) {
    const std::uint32_t w = cb.out_write.load(std::memory_order_relaxed);
    const std::uint32_t r = cb.out_read.load(std::memory_order_acquire);
    if (w - r >= kBridgeRingSlots) {
        return nullptr; // full: host is behind — child drops this output
    }
    return &cb.out_slots[w & (kBridgeRingSlots - 1U)];
}

inline void out_ring_commit(ControlBlock& cb) {
    const std::uint32_t w = cb.out_write.load(std::memory_order_relaxed);
    cb.out_write.store(w + 1U, std::memory_order_release);
}

// Host-side consumer of the output ring. The child writes out_write, so
// it is treated as untrusted: a value that makes (out_write - out_read)
// exceed the ring depth is a corrupt index and is read as "empty",
// never dereferenced (§A.4).
inline const OutSlot* out_ring_peek(ControlBlock& cb) {
    const std::uint32_t r = cb.out_read.load(std::memory_order_relaxed);
    const std::uint32_t w = cb.out_write.load(std::memory_order_acquire);
    const std::uint32_t avail = w - r;
    if (avail == 0U || avail > kBridgeRingSlots) {
        return nullptr; // empty, or a corrupt child index — silence (§A.5)
    }
    return &cb.out_slots[r & (kBridgeRingSlots - 1U)];
}

inline void out_ring_release(ControlBlock& cb) {
    const std::uint32_t r = cb.out_read.load(std::memory_order_relaxed);
    cb.out_read.store(r + 1U, std::memory_order_release);
}

} // namespace nt::ext::bridge
