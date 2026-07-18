// rt/ primitives: correctness under single-threaded protocol use and a
// two-thread stress pass for the queue and triple buffer.
#include "rt/spsc_queue.h"
#include "rt/triple_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>

TEST_CASE("SpscQueue round-trips values in order", "[rt]") {
    nt::rt::SpscQueue<int> q(4);
    REQUIRE(q.capacity() == 4);

    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    REQUIRE(q.push(4));
    REQUIRE_FALSE(q.push(5)); // full

    int v = 0;
    REQUIRE(q.pop(v));
    REQUIRE(v == 1);
    REQUIRE(q.pop(v));
    REQUIRE(v == 2);
    REQUIRE(q.push(5)); // slot freed
    REQUIRE(q.pop(v));
    REQUIRE(v == 3);
    REQUIRE(q.pop(v));
    REQUIRE(v == 4);
    REQUIRE(q.pop(v));
    REQUIRE(v == 5);
    REQUIRE_FALSE(q.pop(v)); // empty
}

TEST_CASE("SpscQueue survives two-thread stress without loss", "[rt]") {
    constexpr std::uint64_t kCount = 200000;
    nt::rt::SpscQueue<std::uint64_t> q(1024);

    std::uint64_t sum = 0;
    std::thread consumer([&] {
        std::uint64_t received = 0;
        std::uint64_t value = 0;
        while (received < kCount) {
            if (q.pop(value)) {
                sum += value;
                ++received;
            }
        }
    });

    for (std::uint64_t i = 1; i <= kCount; ++i) {
        while (!q.push(i)) {
        }
    }
    consumer.join();

    REQUIRE(sum == kCount * (kCount + 1) / 2);
}

TEST_CASE("TripleBuffer reader always sees a complete snapshot", "[rt]") {
    struct Snapshot {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };

    nt::rt::TripleBuffer<Snapshot> tb;

    // Initial read: default snapshot, stable across repeat reads.
    REQUIRE(tb.read().a == 0);
    REQUIRE(tb.read().a == 0);

    tb.write_slot() = {7, 7};
    tb.publish();
    REQUIRE(tb.read().a == 7);

    // Writer thread publishes coherent pairs; reader must never observe
    // a torn snapshot (a != b).
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        std::uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            ++i;
            tb.write_slot() = {i, i};
            tb.publish();
        }
    });

    for (int i = 0; i < 200000; ++i) {
        const Snapshot& s = tb.read();
        REQUIRE(s.a == s.b);
    }
    stop = true;
    writer.join();
}
