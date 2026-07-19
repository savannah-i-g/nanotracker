// ext/bridge/bridge_host_main — the bridge-host CHILD executable.
//
// One instance runs per bridged plugin. It attaches to the host's
// shared-memory segment by name (argv[1]), then runs one of two modes
// (argv[2]):
//   - "echo": multiply input by a gain (argv[3]) — the S29a transport
//     stand-in the pure-transport tests still exercise. Single-threaded.
//   - "clap": open a real .clap (argv[3]) and instantiate a plugin id
//     (argv[4]). The child IS a CLAP host: its RT thread pops input
//     blocks, replays the slot's note/param events onto the ClapPlugin,
//     calls process(), and pushes output — the mirror of the host's
//     wait-free ring traffic. A separate control thread proxies plugin
//     state save/load over the inherited socket (kControlChildFd).
//
// Unlike the host's audio thread, the child MAY block: it is not a
// device callback, so a bounded park while the stream is idle is legal
// (§A.3). During continuous playback it never parks — a fresh input
// block arrives every ~2.67 ms, inside its spin window. State ops run on
// the control thread and serialise against the RT thread with a try-lock,
// so a save/load costs at most a block of child-side silence, never a
// concurrent process() call.
//
// The child links clap_host.cpp + shared_library.cpp unchanged (it is a
// NanoTracker CLAP host with the engine/UI/device stripped away) but none
// of the engine, UI, device, or IO.
#include "ext/bridge/bridge_control_socket.h"
#include "ext/bridge/bridge_protocol.h"
#include "ext/bridge/bridge_shm.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(__linux__)

#include "ext/clap_host.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <sched.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

// ── Echo mode (S29a transport stand-in) ──────────────────────────────
// Pop input, apply the echo/gain transform, push output, bump heartbeat.
void run_echo_loop(ControlBlock& cb, float gain) {
    auto last_work = std::chrono::steady_clock::now();
    const auto park_after = std::chrono::milliseconds(kBridgeParkAfterMs);

    while (cb.shutdown.load(std::memory_order_acquire) == 0) {
        const InSlot* in = in_ring_peek(cb);
        if (in == nullptr) {
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
            cpu_pause();
            continue;
        }
        const std::uint32_t frames = std::min(in->frames, kBridgeBlockFrames);
        const std::uint32_t nsamp = frames * kBridgeChannels;
        out->seq = in->seq;
        out->frames = frames;
        for (std::uint32_t i = 0; i < nsamp; ++i) {
            out->audio_out[i] = in->audio_in[i] * gain;
        }
        out_ring_commit(cb);
        in_ring_release(cb);
        cb.child_heartbeat.fetch_add(1, std::memory_order_release);
        last_work = std::chrono::steady_clock::now();
    }
}

// ── CLAP-hosting mode (S29b) ─────────────────────────────────────────
// State shared between the RT thread (process) and the control thread
// (save/load). The mutex serialises the two so a state op never runs
// concurrently with process(); the RT thread only ever try-locks it, so
// it never blocks — a held lock costs a block of silence, no more.
struct ClapChild {
    nt::ext::ClapPlugin* plugin = nullptr;
    ControlBlock* cb = nullptr;
    std::mutex state_mtx;
};

// The child's real-time thread: the mirror of the host's wait-free path,
// but process() is a real ClapPlugin driven through its GraphPluginBinding
// surface — the same calls the in-process runner makes (graph_runner.cpp:
// 388-443), so the bridged output matches in-process bit for bit.
void run_clap_rt_loop(ClapChild& st) {
    ControlBlock& cb = *st.cb;
    set_denormal_protection();
    request_rt_scheduling();

    auto last_work = std::chrono::steady_clock::now();
    const auto park_after = std::chrono::milliseconds(kBridgeParkAfterMs);

    while (cb.shutdown.load(std::memory_order_acquire) == 0) {
        const InSlot* in = in_ring_peek(cb);
        if (in == nullptr) {
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
            cpu_pause(); // host behind: hold the input, retry
            continue;
        }
        // Never block the RT thread: if a state op holds the lock, hold
        // the input and retry (the host reads silence for this block).
        std::unique_lock<std::mutex> lock(st.state_mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            cpu_pause();
            continue;
        }

        // Replay the block's events onto the plugin, then process. Reset
        // first (drop voices), then notes, then param CV — block-head
        // ordering, matching the in-process runner.
        if (in->reset != 0) {
            st.plugin->plugin_reset();
        }
        const std::uint32_t note_count = std::min(in->note_count, kBridgeMaxNotesPerBlock);
        for (std::uint32_t i = 0; i < note_count; ++i) {
            const NoteEvent& event = in->notes[i];
            if (event.on != 0) {
                st.plugin->plugin_note_on(event.key, event.velocity);
            } else {
                st.plugin->plugin_note_off(event.key);
            }
        }
        const std::uint32_t param_count = std::min(in->param_count, kBridgeMaxParamsPerBlock);
        for (std::uint32_t i = 0; i < param_count; ++i) {
            const ParamChange& change = in->params[i];
            st.plugin->plugin_set_param_cv(static_cast<int>(change.index), change.value01);
        }

        const std::uint32_t frames = std::min(in->frames, kBridgeBlockFrames);
        out->seq = in->seq;
        out->frames = frames;
        // ClapPlugin writes interleaved stereo straight into the output
        // slot; for an instrument it ignores audio_in (its port layout
        // decides), so passing the slot's input is always correct.
        st.plugin->process_block(in->audio_in.data(), out->audio_out.data(), frames);
        lock.unlock();

        out_ring_commit(cb);
        in_ring_release(cb);
        cb.child_heartbeat.fetch_add(1, std::memory_order_release);
        last_work = std::chrono::steady_clock::now();
    }
}

// The control thread (main): blocking request/reply over the inherited
// socket. Save serialises the plugin and returns the blob; load applies a
// blob. Both take the state lock so they never race the RT thread's
// process(), and both run here on the child's main thread — the CLAP
// contract's thread for state I/O. A closed socket (host teardown or
// death) reads EOF and ends the loop, which shuts the child down.
void run_control_loop(int fd, ClapChild& st) {
    for (;;) {
        ControlHeader header{};
        if (!control_read_full(fd, &header, sizeof(header))) {
            return; // EOF or error: host closed the socket
        }
        if (header.magic != kControlMagic || header.length > kControlMaxPayload) {
            return; // desync / hostile length: tear the channel down
        }
        std::vector<std::uint8_t> payload(header.length);
        if (header.length > 0 && !control_read_full(fd, payload.data(), payload.size())) {
            return;
        }

        switch (static_cast<ControlMsg>(header.type)) {
        case ControlMsg::kSaveRequest: {
            std::vector<std::uint8_t> blob;
            {
                const std::lock_guard<std::mutex> lock(st.state_mtx);
                blob = st.plugin->save_state();
            }
            const ControlHeader reply{.magic = kControlMagic,
                                      .type = static_cast<std::uint32_t>(ControlMsg::kSaveReply),
                                      .length = static_cast<std::uint32_t>(blob.size()),
                                      .status = 0};
            if (!control_write_full(fd, &reply, sizeof(reply))) {
                return;
            }
            if (!blob.empty() && !control_write_full(fd, blob.data(), blob.size())) {
                return;
            }
            break;
        }
        case ControlMsg::kLoadRequest: {
            bool applied = false;
            {
                const std::lock_guard<std::mutex> lock(st.state_mtx);
                applied = st.plugin->load_state(payload);
            }
            const ControlHeader reply{.magic = kControlMagic,
                                      .type = static_cast<std::uint32_t>(ControlMsg::kLoadReply),
                                      .length = 0,
                                      .status = applied ? 0U : 1U};
            if (!control_write_full(fd, &reply, sizeof(reply))) {
                return;
            }
            break;
        }
        default:
            return; // unknown request: desync, bail
        }
    }
}

// Open the library + instantiate the plugin (main thread — the CLAP
// thread for load/activate), start the RT thread, then serve state on the
// control thread until the host closes the socket. child_ready is set only
// after the plugin exists, so a load failure is seen by the host as
// not-ready (the child exits without it) → silence + fall back (§A/§C).
int run_clap_child(ControlBlock& cb, const char* library_path, const char* plugin_id) {
    std::string error;
    const std::unique_ptr<nt::ext::ClapLibrary> library =
        nt::ext::ClapLibrary::open(library_path, error);
    if (library == nullptr) {
        std::fprintf(stderr, "bridge-host: open %s: %s\n", library_path, error.c_str());
        return 3; // no child_ready: host sees not-ready and falls back
    }
    const std::unique_ptr<nt::ext::ClapPlugin> plugin =
        nt::ext::ClapPlugin::create(*library, plugin_id, cb.sample_rate, error);
    if (plugin == nullptr) {
        std::fprintf(stderr, "bridge-host: create %s: %s\n", plugin_id, error.c_str());
        return 4;
    }

    ClapChild st;
    st.plugin = plugin.get();
    st.cb = &cb;
    std::thread rt([&st] { run_clap_rt_loop(st); });

    // Plugin created and RT thread live: announce ready (handshake done).
    cb.child_ready.store(1, std::memory_order_release);

    run_control_loop(kControlChildFd, st);

    cb.shutdown.store(1, std::memory_order_release); // in case the socket closed first
    rt.join();
    return 0; // plugin unique_ptr deactivates + destroys on this (main) thread
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <shm-name> <echo|clap> ...\n", argv[0]);
        return 2;
    }
    const std::string name = argv[1];
    const std::string mode = argv[2];

    std::string error;
    const BridgeShm shm = BridgeShm::attach(name, error);
    if (!shm.valid()) {
        std::fprintf(stderr, "bridge-host: %s\n", error.c_str());
        return 3; // handshake failure; the host detects the early exit
    }
    ControlBlock& cb = *shm.control();

    if (mode == "clap") {
        if (argc < 5) {
            std::fprintf(stderr, "usage: %s <shm-name> clap <library-path> <plugin-id>\n", argv[0]);
            return 2;
        }
        return run_clap_child(cb, argv[3], argv[4]);
    }

    if (mode == "echo") {
        if (argc < 4) {
            std::fprintf(stderr, "usage: %s <shm-name> echo <gain>\n", argv[0]);
            return 2;
        }
        const float gain = std::strtof(argv[3], nullptr);
        set_denormal_protection();
        request_rt_scheduling();
        cb.child_ready.store(1, std::memory_order_release);
        run_echo_loop(cb, gain);
        return 0;
    }

    std::fprintf(stderr, "bridge-host: unknown mode '%s'\n", mode.c_str());
    return 2;
}

#else // !__linux__ — the bridge host is Linux-first (§H.2).

int main(int /*argc*/, char** /*argv*/) {
    std::fprintf(stderr, "nanotracker-bridge-host is unsupported on this platform (Linux-first)\n");
    return 1;
}

#endif // __linux__
