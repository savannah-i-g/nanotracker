// rt/triple_buffer — single-writer single-reader latest-value handoff.
// The audio→UI snapshot channel: the writer publishes a whole snapshot
// per block, the reader always sees the most recent complete one.
// Never blocks, never tears, drops intermediate values by design.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace nt::rt {

template <typename T>
class TripleBuffer {
public:
    // Writer: fill a snapshot slot, then publish it.
    T& write_slot() { return slots_[write_index_]; }

    void publish() {
        // Exchange the freshly written slot with the shared slot and
        // flag it as new; the writer continues on the returned slot.
        const uint32_t published = write_index_ | kFreshBit;
        write_index_ = shared_.exchange(published, std::memory_order_acq_rel) & kIndexMask;
    }

    // Reader: returns the most recent published snapshot; when nothing
    // new was published since the last call, the previous snapshot is
    // returned again (stable reads).
    const T& read() {
        const uint32_t shared = shared_.load(std::memory_order_relaxed);
        if ((shared & kFreshBit) != 0) {
            read_index_ = shared_.exchange(read_index_, std::memory_order_acq_rel) & kIndexMask;
        }
        return slots_[read_index_];
    }

private:
    static constexpr uint32_t kFreshBit = 4;
    static constexpr uint32_t kIndexMask = 3;

    std::array<T, 3> slots_{};
    uint32_t write_index_ = 0;
    uint32_t read_index_ = 1;
    std::atomic<uint32_t> shared_{2};
};

} // namespace nt::rt
