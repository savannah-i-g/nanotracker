// rt/spsc_queue — single-producer single-consumer lock-free ring.
// The UI→audio command channel and audio→MIDI event channel. Capacity
// is fixed at construction (rounded up to a power of two); push/pop
// never allocate, lock, or block. T must be trivially copyable — the
// queue is for POD commands, not ownership transfer.
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace nt::rt {

template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue carries POD commands; wrap complex payloads in "
                  "pointers retired on the non-RT side");

public:
    explicit SpscQueue(std::size_t min_capacity) {
        std::size_t cap = 1;
        while (cap < min_capacity) {
            cap <<= 1U;
        }
        buffer_.resize(cap);
        mask_ = cap - 1;
    }

    // Producer side. Returns false when full (caller decides whether to
    // retry next frame or surface an overflow).
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail > mask_) {
            return false;
        }
        buffer_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false when empty.
    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const { return mask_ + 1; }

private:
    std::vector<T> buffer_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace nt::rt
