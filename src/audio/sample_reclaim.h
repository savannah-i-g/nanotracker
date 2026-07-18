// audio/sample_reclaim — deferred reclamation for objects the audio
// thread reads through raw pointers (SampleBuffers, PlaybackBundles,
// FxRacks, GraphRunners). The owning thread retires an object when a
// bundle replacement unlinks it; a per-frame sweep frees it once the
// engine's snapshot stream proves the audio thread can no longer reach
// it. Single-threaded by design: retire/stage/commit/sweep all run on
// the thread that owns the session (the audio thread never touches
// this class — it only ever proves progress through snapshots).
//
// ── The fence ────────────────────────────────────────────────────────
// Grace clock: every kSetBundle/kSwapBundle — and the fenced preview
// commands kPlaySample/kStopSample (ProjectSession::preview_file, which
// retires the buffer a new preview unlinks) — carries a monotonically
// increasing publish serial (Command::serial; the session is the single
// fenced producer). The engine records the serial when it applies the
// command — in the command drain at the head of a pull, before any
// rendering — and republishes the running max in every snapshot
// (EngineSnapshot::bundle_serial), which is written at the END of the
// pull.
//
// Rule: an entry retired by publish S is freeable once a snapshot
// reports bundle_serial >= S.
//
// Why that observation is proof:
//  1. bundle_serial >= S in a snapshot means the pull that applied
//     publish S (or a later publish) has COMPLETED — the snapshot is
//     written after the pull's rendering, and the serial only enters
//     the snapshot through that apply.
//  2. Pulls execute strictly in order on one render thread, so every
//     pull preceding the applying pull has also completed. Those are
//     the only pulls that could have rendered from the replaced
//     bundle.
//  3. From the applying pull onward the engine holds the new bundle
//     for the whole pull (drain precedes rendering): kSetBundle
//     deactivates every voice class that holds a SampleBuffer*
//     (channel, sequence-layer, one-shot preview, and — via
//     plugin_reset() on every bound instance — plugin-internal
//     sampler voices, whose zone/slice override buffers retire
//     through this same fence) in the same drain,
//     so no voice carries a stale pointer across the swap; kSwapBundle
//     keeps voices alive but its sender contract (audio_engine.h)
//     requires an identical project and sample set, so surviving
//     voices reference only buffers the new bundle still holds.
//     Commands that carry sample pointers (preview, play-sample) share
//     the FIFO with the replacement, so none drained after the apply
//     can resurrect a retired pointer.
//  Hence at observation time nothing on the audio thread can reach the
//  retired object, now or in any later pull.
//
// A raw pull-counter grace period (free at observed pulls >= retire
// pulls + margin) was considered and rejected: the UI-side snapshot
// read is wait-free, not fresh (rt/triple_buffer.h), so it may lag the
// render thread by an unbounded number of pulls — a retire-time
// `pulls` stamp can under-count in-flight pulls and no fixed margin
// closes the race. The applied-serial echo is exact, and snapshot
// staleness can only delay a free, never permit an early one.
//
// Delivery precondition: the fence assumes the replacement command was
// actually enqueued. AudioEngine::send drops when the queue is full,
// so the session disarms sweeping for its remaining lifetime the
// moment the engine's drop counter moves — fail-safe to
// keep-until-shutdown (a leak, never a use-after-free). An engine that
// never pulls frees nothing until destruction; the destructor frees
// everything unconditionally (by then the device is stopped or the
// session gone, per the existing shutdown order).
//
// Two-phase retirement: an object can be unlinked from the session's
// tables before the replacement publish exists (sample decode swaps
// buffers_ slots, then the bundle republishes later in the same
// operation). stage() parks such objects without a stamp;
// commit_staged() stamps them with the publish that finally removed
// the audio thread's route to them. Staged entries are never swept.
//
// Entries hold shared_ptr<void>: destructive-edit undo snapshots alias
// the same buffers, so `freed` counts entries whose reclaimer
// reference the fence released — the last owner (usually this class,
// sometimes a surviving undo entry) runs the destructor.
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace nt::audio {

class SampleReclaimer {
public:
    // Takes ownership; `retired_at_serial` is the publish whose apply
    // removes the audio thread's last route to the object.
    template <typename T>
    void retire(std::shared_ptr<T> object, std::uint64_t retired_at_serial) {
        if (object == nullptr) {
            return;
        }
        entries_.push_back({std::shared_ptr<void>(std::move(object)), retired_at_serial});
        ++retired_;
    }

    template <typename T>
    void retire(std::unique_ptr<T> object, std::uint64_t retired_at_serial) {
        retire(std::shared_ptr<T>(std::move(object)), retired_at_serial);
    }

    // Parks an object that is already unlinked from the session's own
    // tables but still reachable through the live bundle; the next
    // commit_staged() stamps it.
    template <typename T>
    void stage(std::shared_ptr<T> object) {
        if (object != nullptr) {
            staged_.push_back(std::shared_ptr<void>(std::move(object)));
        }
    }

    template <typename T>
    void stage(std::unique_ptr<T> object) {
        stage(std::shared_ptr<T>(std::move(object)));
    }

    void commit_staged(std::uint64_t retired_at_serial);

    // Releases every committed entry whose retirement serial the audio
    // thread has provably applied (see the fence above). No-op after
    // disarm().
    void sweep(std::uint64_t observed_serial);

    // Permanently pins entries until destruction — the fail-safe when
    // command delivery can no longer be proven (a dropped command).
    void disarm() { disarmed_ = true; }

    [[nodiscard]] bool disarmed() const { return disarmed_; }

    [[nodiscard]] std::uint64_t retired_count() const { return retired_; }

    [[nodiscard]] std::uint64_t freed_count() const { return freed_; }

    // Committed + staged entries still held.
    [[nodiscard]] std::size_t live_count() const { return entries_.size() + staged_.size(); }

    [[nodiscard]] std::size_t staged_count() const { return staged_.size(); }

private:
    struct Entry {
        std::shared_ptr<void> object;
        std::uint64_t retired_at;
    };

    std::vector<Entry> entries_;
    std::vector<std::shared_ptr<void>> staged_;
    std::uint64_t retired_ = 0;
    std::uint64_t freed_ = 0;
    bool disarmed_ = false;
};

} // namespace nt::audio
