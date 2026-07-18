// SampleReclaimer fence semantics against a fake applied-serial
// sequence: destructor probes prove nothing frees before its serial is
// observed, staging defers the stamp to the committing publish, and
// disarm pins everything until destruction.
#include "audio/sample_reclaim.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

// Sets its flag on destruction — the observable "freed" event.
class Probe {
public:
    explicit Probe(bool* destroyed) : destroyed_(destroyed) {}

    ~Probe() { *destroyed_ = true; }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    Probe(Probe&&) = delete;
    Probe& operator=(Probe&&) = delete;

private:
    bool* destroyed_;
};

} // namespace

TEST_CASE("entries free only once their serial is observed", "[reclaim]") {
    bool freed_a = false;
    bool freed_b = false;
    nt::audio::SampleReclaimer reclaimer;
    reclaimer.retire(std::make_unique<Probe>(&freed_a), 1);
    reclaimer.retire(std::make_unique<Probe>(&freed_b), 2);
    CHECK(reclaimer.retired_count() == 2);
    CHECK(reclaimer.live_count() == 2);

    reclaimer.sweep(0); // nothing applied yet
    CHECK(!freed_a);
    CHECK(!freed_b);
    CHECK(reclaimer.freed_count() == 0);

    reclaimer.sweep(1); // publish 1 provably applied
    CHECK(freed_a);
    CHECK(!freed_b);
    CHECK(reclaimer.freed_count() == 1);
    CHECK(reclaimer.live_count() == 1);

    reclaimer.sweep(1); // idempotent on a stalled clock
    CHECK(!freed_b);

    reclaimer.sweep(5); // later observation frees older serials
    CHECK(freed_b);
    CHECK(reclaimer.freed_count() == 2);
    CHECK(reclaimer.live_count() == 0);
}

TEST_CASE("staged entries are unsweepable until committed", "[reclaim]") {
    bool freed = false;
    nt::audio::SampleReclaimer reclaimer;
    reclaimer.stage(std::make_unique<Probe>(&freed));
    CHECK(reclaimer.staged_count() == 1);
    CHECK(reclaimer.live_count() == 1);

    // Staged = unlinked from the session but still reachable through
    // the live bundle; no observation may free it.
    reclaimer.sweep(100);
    CHECK(!freed);

    reclaimer.commit_staged(101); // the publish that unlinks it
    CHECK(reclaimer.staged_count() == 0);
    CHECK(reclaimer.retired_count() == 1);
    reclaimer.sweep(100);
    CHECK(!freed);
    reclaimer.sweep(101);
    CHECK(freed);
    CHECK(reclaimer.freed_count() == 1);
}

TEST_CASE("disarm pins entries until destruction", "[reclaim]") {
    bool freed = false;
    {
        nt::audio::SampleReclaimer reclaimer;
        reclaimer.retire(std::make_unique<Probe>(&freed), 1);
        reclaimer.disarm();
        reclaimer.sweep(100);
        CHECK(!freed); // fail-safe: leak, never a use-after-free
        CHECK(reclaimer.freed_count() == 0);
    }
    CHECK(freed); // destruction releases unconditionally
}

TEST_CASE("shared snapshots defer deletion to the last owner", "[reclaim]") {
    bool freed = false;
    auto snapshot = std::make_shared<Probe>(&freed);
    nt::audio::SampleReclaimer reclaimer;
    reclaimer.retire(std::shared_ptr<Probe>(snapshot), 3);
    reclaimer.sweep(3);
    // The fence released the reclaimer's reference ("freed"), but an
    // undo snapshot still owns the object.
    CHECK(reclaimer.freed_count() == 1);
    CHECK(!freed);
    snapshot.reset();
    CHECK(freed);
}
