// VST3 IRunLoop mechanics (ext/vst3_run_loop): fd handlers fire when
// their descriptor is readable, timers fire on their deadline, and the
// two Research/07 host-bug guards hold — unregister-during-dispatch
// never corrupts the pass, and a re-entrant dispatch from a callback
// returns without re-firing. Windows has no IRunLoop (global message
// loop), so this suite compiles away there with the implementation.
#ifndef _WIN32

#include "ext/vst3_run_loop.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <functional>
#include <thread>
#include <unistd.h>

namespace {

using namespace Steinberg;

// Minimal FUnknown scaffolding for plugin-side handler fakes: the run
// loop must addRef registered handlers and release them once they
// leave (asserted via `refs`).
template <typename Interface>
class FakeHandlerBase : public Interface {
public:
    tresult PLUGIN_API queryInterface(const TUID /*iid*/, void** obj) override {
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return ++refs; }

    uint32 PLUGIN_API release() override { return --refs; }

    uint32 refs = 0;
};

class FakeFdHandler final : public FakeHandlerBase<Linux::IEventHandler> {
public:
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor fd) override {
        ++fired;
        last_fd = fd;
        if (on_fire) {
            on_fire();
        }
    }

    int fired = 0;
    int last_fd = -1;
    std::function<void()> on_fire;
};

class FakeTimerHandler final : public FakeHandlerBase<Linux::ITimerHandler> {
public:
    void PLUGIN_API onTimer() override {
        ++fired;
        if (on_fire) {
            on_fire();
        }
    }

    int fired = 0;
    std::function<void()> on_fire;
};

// Self-pipe pair, closed on scope exit.
struct Pipe {
    Pipe() { REQUIRE(pipe(fds.data()) == 0); }

    ~Pipe() {
        close(fds[0]);
        close(fds[1]);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    void make_readable() const { REQUIRE(write(fds[1], "x", 1) == 1); }

    void drain() const {
        std::array<char, 16> sink{};
        const ssize_t drained = read(fds[0], sink.data(), sink.size());
        CHECK(drained > 0);
    }

    [[nodiscard]] int read_fd() const { return fds[0]; }

    std::array<int, 2> fds{-1, -1};
};

} // namespace

TEST_CASE("run loop fires fd handlers on readable descriptors only", "[vst3][runloop]") {
    auto& loop = nt::ext::Vst3RunLoop::instance();
    Pipe pipe;
    FakeFdHandler handler;
    REQUIRE(loop.registerEventHandler(&handler, pipe.read_fd()) == kResultOk);
    CHECK(handler.refs == 1); // registration holds a reference

    loop.dispatch();
    CHECK(handler.fired == 0); // nothing written yet

    pipe.make_readable();
    loop.dispatch();
    CHECK(handler.fired == 1);
    CHECK(handler.last_fd == pipe.read_fd());

    // Undrained descriptor: still readable, fires again next frame
    // (level-triggered poll, matching the CLAP pump).
    loop.dispatch();
    CHECK(handler.fired == 2);
    pipe.drain();
    loop.dispatch();
    CHECK(handler.fired == 2);

    REQUIRE(loop.unregisterEventHandler(&handler) == kResultOk);
    CHECK(handler.refs == 0); // reference released on removal
    CHECK(loop.handler_count() == 0);

    // Null/invalid registrations are refused, not stored.
    CHECK(loop.registerEventHandler(nullptr, pipe.read_fd()) == kInvalidArgument);
    CHECK(loop.registerEventHandler(&handler, -1) == kInvalidArgument);
    CHECK(loop.unregisterEventHandler(&handler) == kResultFalse);
}

TEST_CASE("run loop fires timers on their deadline, once per pass", "[vst3][runloop]") {
    auto& loop = nt::ext::Vst3RunLoop::instance();
    FakeTimerHandler timer;
    REQUIRE(loop.registerTimer(&timer, 10) == kResultOk);
    CHECK(timer.refs == 1);

    loop.dispatch();
    CHECK(timer.fired == 0); // deadline not reached

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    loop.dispatch();
    CHECK(timer.fired == 1);
    loop.dispatch(); // deadline advanced — no double fire in a frame
    CHECK(timer.fired == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    loop.dispatch();
    CHECK(timer.fired == 2);

    REQUIRE(loop.unregisterTimer(&timer) == kResultOk);
    CHECK(timer.refs == 0);
    CHECK(loop.handler_count() == 0);
}

TEST_CASE("unregister during dispatch is safe and honoured", "[vst3][runloop]") {
    auto& loop = nt::ext::Vst3RunLoop::instance();
    Pipe pipe_a;
    Pipe pipe_b;
    FakeFdHandler first;
    FakeFdHandler second;
    // The first handler unregisters BOTH handlers from inside its
    // callback — the classic mutation-during-iteration crash in naive
    // hosts. The pass must complete and the second handler must not
    // fire after its removal.
    first.on_fire = [&] {
        CHECK(loop.unregisterEventHandler(&first) == kResultOk);
        CHECK(loop.unregisterEventHandler(&second) == kResultOk);
    };
    REQUIRE(loop.registerEventHandler(&first, pipe_a.read_fd()) == kResultOk);
    REQUIRE(loop.registerEventHandler(&second, pipe_b.read_fd()) == kResultOk);

    pipe_a.make_readable();
    pipe_b.make_readable();
    loop.dispatch();
    CHECK(first.fired == 1);
    CHECK(second.fired == 0); // tombstoned mid-pass, skipped
    CHECK(loop.handler_count() == 0);
    CHECK(first.refs == 0);
    CHECK(second.refs == 0);
}

TEST_CASE("register during dispatch waits for the next pass", "[vst3][runloop]") {
    auto& loop = nt::ext::Vst3RunLoop::instance();
    Pipe pipe_a;
    Pipe pipe_b;
    FakeFdHandler outer;
    FakeFdHandler inner;
    outer.on_fire = [&] {
        if (outer.fired == 1) {
            CHECK(loop.registerEventHandler(&inner, pipe_b.read_fd()) == kResultOk);
        }
    };
    REQUIRE(loop.registerEventHandler(&outer, pipe_a.read_fd()) == kResultOk);

    pipe_a.make_readable();
    pipe_b.make_readable();
    loop.dispatch();
    CHECK(outer.fired == 1);
    CHECK(inner.fired == 0); // appended past the pass snapshot
    loop.dispatch();
    CHECK(inner.fired == 1);

    pipe_a.drain();
    CHECK(loop.unregisterEventHandler(&outer) == kResultOk);
    CHECK(loop.unregisterEventHandler(&inner) == kResultOk);
    CHECK(loop.handler_count() == 0);
}

TEST_CASE("re-entrant dispatch from a timer callback never re-fires", "[vst3][runloop]") {
    auto& loop = nt::ext::Vst3RunLoop::instance();
    FakeTimerHandler timer;
    timer.on_fire = [&] {
        loop.dispatch(); // a plugin pumping the loop from its callback
        loop.dispatch();
    };
    REQUIRE(loop.registerTimer(&timer, 1) == kResultOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    loop.dispatch();
    // The Cockos-class re-entrancy bug would stack callbacks here.
    CHECK(timer.fired == 1);
    CHECK(loop.unregisterTimer(&timer) == kResultOk);
    CHECK(loop.handler_count() == 0);
}

#endif // !_WIN32
