// ext/bridge/bridge_host_main — the bridge-host CHILD executable.
//
// One instance runs per bridged plugin. It attaches to the host's
// shared-memory segment by name (argv[1]), completes the version-first
// handshake, then runs a real-time thread that pops input blocks,
// applies process(), and pushes output — the mirror image of the host's
// wait-free ring traffic. In S29a "process()" is a trivial echo/gain
// stand-in (argv[2]); S29b swaps in a real ClapPlugin behind the same
// loop. The child links none of the engine, UI, device, or IO.
//
// Unlike the host's audio thread, the child MAY block: it is not a
// device callback, so a bounded park while the stream is idle is legal
// and saves the core (§A.3). During continuous playback it never parks —
// a fresh input block arrives every ~2.67 ms, inside its spin window.
#include "ext/bridge/bridge_protocol.h"
#include "ext/bridge/bridge_shm.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__linux__)

#include <chrono>
#include <sched.h>
#include <unistd.h>

#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define NT_HAVE_SSE 1 // NOLINT(cppcoreguidelines-macro-usage) — feature-test macro
#endif

namespace {

using namespace nt::ext::bridge;

// Denormal protection on the child DSP thread (doctrine line 18): the
// same FTZ/DAZ the in-process render thread sets (audio_engine.cpp:20).
void set_denormal_protection() {
#ifdef NT_HAVE_SSE
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// Best-effort real-time scheduling (§H.3): request SCHED_FIFO and fall
// back silently to normal scheduling when the rtprio limit denies it
// (the standard Linux-audio posture — the feature works out of the box,
// raising limits is documented power-user tuning). Host audio-priority
// propagation is deferred tuning (§G.4); a modest fixed priority here.
void request_rt_scheduling() {
    sched_param param{};
    const int lo = ::sched_get_priority_min(SCHED_FIFO);
    const int hi = ::sched_get_priority_max(SCHED_FIFO);
    if (lo < 0 || hi < 0) {
        return;
    }
    param.sched_priority = lo + ((hi - lo) / 2);
    ::sched_setscheduler(0, SCHED_FIFO, &param); // ignore EPERM: silent fallback
}

void cpu_pause() {
#ifdef NT_HAVE_SSE
    _mm_pause();
#endif
}

// Adaptive park: declare parked, re-check for work / shutdown to avoid a
// lost wakeup, then futex-wait on the host's wake word (with a timeout so
// shutdown is re-polled even if a wake is somehow missed).
void park_until_woken(ControlBlock& cb) {
    cb.child_parked.store(1, std::memory_order_release);
    if (in_ring_peek(cb) != nullptr || cb.shutdown.load(std::memory_order_acquire) != 0) {
        cb.child_parked.store(0, std::memory_order_release);
        return;
    }
    const std::uint32_t expected = cb.wake.load(std::memory_order_acquire);
    bridge_futex_wait(cb.wake, expected, kBridgeParkTimeoutNs);
    cb.child_parked.store(0, std::memory_order_release);
}

// The child RT loop: pop input, apply the S29a echo/gain transform, push
// output, bump the heartbeat. Mirror of the host's wait-free path.
void run_child_loop(ControlBlock& cb, float gain) {
    auto last_work = std::chrono::steady_clock::now();
    const auto park_after = std::chrono::milliseconds(kBridgeParkAfterMs);

    while (cb.shutdown.load(std::memory_order_acquire) == 0) {
        const InSlot* in = in_ring_peek(cb);
        if (in == nullptr) {
            // No work: spin hot, park only after an idle window (§A.3).
            if (std::chrono::steady_clock::now() - last_work > park_after) {
                park_until_woken(cb);
                last_work = std::chrono::steady_clock::now();
            } else {
                cpu_pause();
            }
            continue;
        }
        OutSlot* out = out_ring_acquire(cb);
        if (out == nullptr) {
            // Output ring full: the host is behind. Hold the input and
            // spin; the top-of-loop shutdown check keeps this bounded.
            cpu_pause();
            continue;
        }
        const std::uint32_t frames = std::min(in->frames, kBridgeBlockFrames);
        const std::uint32_t nsamp = frames * kBridgeChannels;
        out->seq = in->seq;
        out->frames = frames;
        for (std::uint32_t i = 0; i < nsamp; ++i) {
            out->audio_out[i] = in->audio_in[i] * gain; // S29a stand-in for process()
        }
        out_ring_commit(cb);
        in_ring_release(cb);
        cb.child_heartbeat.fetch_add(1, std::memory_order_release);
        last_work = std::chrono::steady_clock::now();
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <shm-name> <echo-gain>\n", argv[0]);
        return 2;
    }
    const std::string name = argv[1];
    const float gain = std::strtof(argv[2], nullptr);

    std::string error;
    const BridgeShm shm = BridgeShm::attach(name, error);
    if (!shm.valid()) {
        std::fprintf(stderr, "bridge-host: %s\n", error.c_str());
        return 3; // handshake failure; the host detects the early exit
    }
    ControlBlock& cb = *shm.control();

    set_denormal_protection();
    request_rt_scheduling();

    // Handshake complete (attach validated the header): announce ready.
    cb.child_ready.store(1, std::memory_order_release);

    run_child_loop(cb, gain);
    return 0;
}

#else // !__linux__ — the bridge host is Linux-first (§H.2).

int main(int /*argc*/, char** /*argv*/) {
    std::fprintf(stderr, "nanotracker-bridge-host is unsupported on this platform (Linux-first)\n");
    return 1;
}

#endif // __linux__
