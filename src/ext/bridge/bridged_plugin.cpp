#include "ext/bridge/bridged_plugin.h"

#include "ext/bridge/bridge_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#if defined(__linux__)

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h> // declares environ, used by posix_spawn below

#endif // __linux__

namespace nt::ext::bridge {

// The ring's block geometry must match the graph's, or a full block
// would over/under-run a slot. Enforced here, where both headers meet.
static_assert(kBridgeBlockFrames == audio::kGraphBlockFrames,
              "bridge block frames must equal the graph block size");

#if defined(__linux__)

namespace {

namespace fs = std::filesystem;

// Locates the bridge-host child binary. An explicit path wins;
// $NT_BRIDGE_HOST_EXE overrides for tests / power users (mirroring
// $NANOTRACKER_ASSETS); otherwise it sits beside this executable. The
// /proc/self/exe read duplicates platform/paths.cpp's private
// executable_dir() rather than widening that module's public surface,
// keeping the binding self-contained.
fs::path resolve_host_exe(const fs::path& explicit_path) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    if (const char* env = std::getenv("NT_BRIDGE_HOST_EXE"); env != nullptr && *env != '\0') {
        return fs::path(env); // NOLINT(modernize-return-braced-init-list) — clearer than {env}
    }
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return {};
    }
    return exe.parent_path() / "nanotracker-bridge-host";
}

std::string make_segment_name() {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — process-wide unique
    // suffix
    static std::atomic<std::uint32_t> counter{0};
    const std::uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
    return "/nt-bridge-" + std::to_string(::getpid()) + "-" + std::to_string(n);
}

void sleep_ns(long ns) {
    const timespec req{.tv_sec = 0, .tv_nsec = ns};
    ::nanosleep(&req, nullptr);
}

} // namespace

std::unique_ptr<BridgedPlugin> BridgedPlugin::spawn(const Config& config, std::string& error) {
    const fs::path host_exe = resolve_host_exe(config.host_exe);
    if (host_exe.empty()) {
        error = "could not resolve the nanotracker-bridge-host executable path";
        return nullptr;
    }
    std::error_code ec;
    if (!fs::exists(host_exe, ec)) {
        error = "bridge-host executable not found: " + host_exe.string();
        return nullptr;
    }

    BridgeShm shm = BridgeShm::create(make_segment_name(), config.sample_rate, kBridgeChannels,
                                      kBridgeChannels, error);
    if (!shm.valid()) {
        return nullptr; // error already set
    }

    // argv: [exe, shm-name, echo-gain]. Strings outlive posix_spawn.
    std::string exe_str = host_exe.string();
    std::string name_str = shm.name();
    std::string gain_str = std::to_string(config.echo_gain);
    std::array<char*, 4> argv{exe_str.data(), name_str.data(), gain_str.data(), nullptr};

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe_str.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        error = "posix_spawn failed: " + std::string(std::strerror(rc));
        return nullptr; // shm destructor unlinks the segment
    }

    // Wait (off the audio thread, so blocking is fine) for the child to
    // validate the header and signal ready, or for it to exit early on a
    // handshake failure. Bounded so a wedged child never hangs startup.
    const ControlBlock& cb = *shm.control();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cb.child_ready.load(std::memory_order_acquire) != 0) {
            ready = true;
            break;
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            error = "bridge-host child exited during handshake";
            return nullptr; // reaped; shm destructor unlinks
        }
        sleep_ns(200'000); // 200 us
    }
    if (!ready) {
        error = "bridge-host child did not become ready";
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        return nullptr;
    }

    std::unique_ptr<BridgedPlugin> plugin(new BridgedPlugin(std::move(shm), pid, config));
    plugin->notify_active(); // in case the child parked before the first push
    return plugin;
}

void BridgedPlugin::teardown() noexcept {
    if (pid_ <= 0) {
        return;
    }
    if (shm_.valid()) {
        ControlBlock& cb = *shm_.control();
        cb.shutdown.store(1, std::memory_order_release);
        notify_active(); // wake a parked child so it observes shutdown
    }
    // Bounded graceful reap, then SIGKILL anything that overstays (§B.5).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool reaped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_ || (r < 0 && errno == ECHILD)) {
            reaped = true;
            break;
        }
        sleep_ns(1'000'000); // 1 ms
    }
    if (!reaped) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
    }
    pid_ = -1;
}

bool BridgedPlugin::running() const {
    // Non-reaping existence probe; real liveness confirmation is S29c's
    // reaper. Signal 0 checks only whether the pid is deliverable.
    return pid_ > 0 && ::kill(pid_, 0) == 0;
}

#else // !__linux__ — the whole bridge is a no-op on non-Linux (§H.2).

std::unique_ptr<BridgedPlugin> BridgedPlugin::spawn(const Config& /*config*/, std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return nullptr;
}

void BridgedPlugin::teardown() noexcept {}

bool BridgedPlugin::running() const {
    return false;
}

#endif // __linux__

BridgedPlugin::BridgedPlugin(BridgeShm shm, int pid, const Config& config)
    : shm_(std::move(shm)), pid_(pid), has_audio_input_(config.has_audio_input) {}

BridgedPlugin::~BridgedPlugin() {
    teardown();
}

// ── The wait-free host callback path (§A.1) ──────────────────────────
// Every statement below is one of: an atomic load/store on the mlocked
// segment, a bounded memcpy/fill over that segment, or an integer op.
// There is no allocation, no syscall, no lock, and no unbounded loop —
// under any child state (fresh / slow / hung / dead) the worst case is
// one block of silence via an empty output ring. This is the entire
// RT-safety argument, made mechanical; the alloc-free property is
// asserted under an RtScope in the tests.
void BridgedPlugin::process_block(const float* in, float* out, std::uint32_t frames) {
    ControlBlock& cb = *shm_.control();
    const std::uint32_t nsamp = frames * kBridgeChannels;

    // 1) Publish this block's input. If the ring is full the child is
    //    behind: drop the block (no retry, no wait) — the empty output
    //    ring already yields silence for it (§A.5).
    if (InSlot* slot = in_ring_acquire(cb)) {
        slot->seq = seq_++;
        slot->frames = frames;
        slot->reset = 0;
        slot->transport = Transport{};
        slot->note_count = 0; // event marshalling is S29b
        slot->param_count = 0;
        if (in != nullptr) {
            std::memcpy(slot->audio_in.data(), in, static_cast<std::size_t>(nsamp) * sizeof(float));
        } else {
            std::fill_n(slot->audio_in.data(), nsamp, 0.0F); // instrument: no dry input
        }
        in_ring_commit(cb);
    }

    // 2) Consume the oldest ready output (the input from one block
    //    earlier). None ready -> silence for this block; the child's
    //    frame count is clamped since the child is untrusted (§A.4).
    if (const OutSlot* slot = out_ring_peek(cb)) {
        const std::uint32_t valid = std::min(slot->frames, frames) * kBridgeChannels;
        std::memcpy(out, slot->audio_out.data(), static_cast<std::size_t>(valid) * sizeof(float));
        if (valid < nsamp) {
            std::fill_n(out + valid, nsamp - valid, 0.0F);
        }
        out_ring_release(cb);
    } else {
        std::fill_n(out, nsamp, 0.0F);
    }
}

void BridgedPlugin::notify_active() {
    if (!shm_.valid()) {
        return;
    }
    ControlBlock& cb = *shm_.control();
    // Bump the futex word (so a child mid-park sees a changed value and
    // does not lose the wake) then wake it. Both are off the audio
    // thread; harmless when the child is already spinning.
    cb.wake.fetch_add(1, std::memory_order_release);
    bridge_futex_wake(cb.wake);
}

std::uint64_t BridgedPlugin::heartbeat() const {
    if (!shm_.valid()) {
        return 0;
    }
    return shm_.control()->child_heartbeat.load(std::memory_order_acquire);
}

bool BridgedPlugin::output_pending() const {
    if (!shm_.valid()) {
        return false;
    }
    const ControlBlock& cb = *shm_.control();
    const std::uint32_t r = cb.out_read.load(std::memory_order_relaxed);
    const std::uint32_t w = cb.out_write.load(std::memory_order_acquire);
    const std::uint32_t avail = w - r;
    return avail != 0 && avail <= kBridgeRingSlots;
}

} // namespace nt::ext::bridge
