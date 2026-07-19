#include "ext/bridge/bridged_plugin.h"

#include "ext/bridge/bridge_control_socket.h"
#include "ext/bridge/bridge_protocol.h"
#include "ext/editor_host_surface.h"

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
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h> // declares environ, used by posix_spawn below

#endif // __linux__

namespace nt::ext::bridge {

// The ring's block geometry must match the graph's, or a full block
// would over/under-run a slot. Enforced here, where both headers meet.
static_assert(kBridgeBlockFrames == audio::kGraphBlockFrames,
              "bridge block frames must equal the graph block size");

// Heartbeat-staleness bypass threshold (§B.4 signal 1). The audio thread
// switches a bridged node's OUTPUT to bypass after this many consecutive
// blocks with no child_heartbeat advance. 32 blocks is ~85 ms at 48 kHz /
// 128 frames — an order of magnitude beyond any legitimate single-block
// scheduling stall (sub-ms to a few ms), so a merely-slow or jittery child
// is never misread as dead, yet short enough that a real crash flips to
// bypass output on its own even when the session-thread reaper is not being
// polled. When the reaper IS polled (per app-loop frame) it usually
// confirms + latches kCrashed first; this threshold is the audio thread's
// self-sufficient backstop. It is NOT a death confirmation — only the
// reaper confirms death; a child that resumes heartbeating recovers (§C.1).
inline constexpr std::uint32_t kBypassStaleBlocks = 32;

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

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

std::unique_ptr<BridgedPlugin> BridgedPlugin::spawn(const Config& config, std::string& error) {
    BridgeShm shm = BridgeShm::create(make_segment_name(), config.sample_rate, kBridgeChannels,
                                      kBridgeChannels, error);
    if (!shm.valid()) {
        return nullptr; // error already set
    }
    // The binding owns the segment from here: if spawn_child fails, its
    // destructor unmaps + unlinks it (pid_ stays -1, so teardown no-ops).
    std::unique_ptr<BridgedPlugin> plugin(new BridgedPlugin(std::move(shm), config));
    if (!plugin->spawn_child(error)) {
        return nullptr;
    }
    plugin->notify_active(); // in case the child parked before the first push
    return plugin;
}

// Spawn (or, from restart(), respawn) the child against the already-mapped
// segment. Off the audio thread, so blocking is fine. Sets pid_ + control_fd_
// on success; leaves them at -1 and sets `error` on failure.
bool BridgedPlugin::spawn_child(std::string& error) {
    const fs::path host_exe = resolve_host_exe(config_.host_exe);
    if (host_exe.empty()) {
        error = "could not resolve the nanotracker-bridge-host executable path";
        return false;
    }
    std::error_code ec;
    if (!fs::exists(host_exe, ec)) {
        error = "bridge-host executable not found: " + host_exe.string();
        return false;
    }

    // CLAP-hosting mode carries a plugin path/id and a control socket for
    // state; echo mode carries a gain and no socket (the pure-transport
    // S29a tests). Build the mode's argv and, when hosting, a socketpair
    // whose child end is dup'd onto the conventional control fd.
    const bool clap_mode = !config_.plugin_path.empty();
    int host_fd = -1;  // host end of the control socket (kept; -1 in echo mode)
    int child_fd = -1; // child end (dup'd to kControlChildFd, then closed here)
    if (clap_mode) {
        std::array<int, 2> sv{-1, -1};
        // SOCK_CLOEXEC so neither end leaks into unrelated spawns; the
        // child end is re-materialised (non-CLOEXEC) via adddup2 below.
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()) != 0) {
            error = "socketpair failed: " + std::string(std::strerror(errno));
            return false;
        }
        host_fd = sv[0];
        child_fd = sv[1];
    }

    // argv: echo = [exe, shm-name, "echo", gain]; clap = [exe, shm-name,
    // "clap", plugin-path, plugin-id]. Strings outlive posix_spawn.
    std::string exe_str = host_exe.string();
    std::string name_str = shm_.name();
    std::string mode_str = clap_mode ? "clap" : "echo";
    std::string arg3 = clap_mode ? config_.plugin_path.string() : std::to_string(config_.echo_gain);
    std::string arg4 = config_.plugin_id;
    std::array<char*, 6> argv{};
    argv[0] = exe_str.data();
    argv[1] = name_str.data();
    argv[2] = mode_str.data();
    argv[3] = arg3.data();
    argv[4] = clap_mode ? arg4.data() : nullptr;
    argv[5] = nullptr;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t* actions_ptr = nullptr;
    if (clap_mode) {
        posix_spawn_file_actions_init(&actions);
        // Place the child's socket end on the conventional descriptor;
        // dup2 clears CLOEXEC on the target, so it survives the exec.
        posix_spawn_file_actions_adddup2(&actions, child_fd, kControlChildFd);
        actions_ptr = &actions;
    }

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe_str.c_str(), actions_ptr, nullptr, argv.data(), environ);
    if (actions_ptr != nullptr) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (rc != 0) {
        error = "posix_spawn failed: " + std::string(std::strerror(rc));
        close_fd(host_fd);
        close_fd(child_fd);
        return false;
    }
    close_fd(child_fd); // the child now owns its dup; the host keeps host_fd

    // Wait (off the audio thread, so blocking is fine) for the child to
    // create the plugin and signal ready, or for it to exit early on a
    // handshake / load failure. Bounded so a wedged child never hangs
    // startup, and a child that fails to load is seen as not-ready.
    const ControlBlock& cb = *shm_.control();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cb.child_ready.load(std::memory_order_acquire) != 0) {
            ready = true;
            break;
        }
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            error = "bridge-host child exited during handshake (plugin load failed?)";
            close_fd(host_fd);
            return false; // reaped
        }
        sleep_ns(200'000); // 200 us
    }
    if (!ready) {
        error = "bridge-host child did not become ready";
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        close_fd(host_fd);
        return false;
    }

    pid_ = pid;
    control_fd_ = host_fd;
    return true;
}

void BridgedPlugin::teardown() noexcept {
    // Close any open editor first: tell the live child to destroy its editor
    // (gui before window) and drop the host container, before the child is
    // shut down below. A no-op when none is open or the child is already dead.
    close_editor();
    // A crash-reaped child leaves pid_ == -1 but its dead control socket
    // still open (poll_liveness reaps the pid, not the fd) — always close
    // it so a never-restarted crashed node does not leak the descriptor.
    if (pid_ <= 0) {
        close_fd(control_fd_);
        return;
    }
    if (shm_.valid()) {
        ControlBlock& cb = *shm_.control();
        cb.shutdown.store(1, std::memory_order_release);
        notify_active(); // wake a parked child so it observes shutdown
    }
    // Closing the host end of the control socket gives the child's control
    // thread EOF, unblocking its blocking read so it can join its RT thread
    // and exit (the RT thread already saw shutdown above).
    close_fd(control_fd_);
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
    // Non-reaping existence probe (diagnostics); poll_liveness is the real
    // reaper. Signal 0 checks only whether the pid is deliverable.
    return pid_ > 0 && ::kill(pid_, 0) == 0;
}

// ── Crash detection: the session-thread reaper (§B.4 signals 2 + 3) ──
bool BridgedPlugin::poll_liveness() {
    // Once latched crashed, nothing to do until restart() clears it.
    if (state_.load(std::memory_order_acquire) == LiveState::kCrashed) {
        return false;
    }
    if (pid_ <= 0) {
        return false; // no live child to reap
    }
    // Signal 2: the reaper. waitpid(WNOHANG) both confirms death and reaps
    // the zombie. (pidfd_open + poll is the richer Linux>=5.3 route; waitpid
    // is chosen here — we own the pid and want no extra fd nor a
    // process-global SIGCHLD handler to coordinate. It is the equivalent
    // confirmation, and never blocks.)
    bool dead = false;
    bool reaped = false;
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == pid_ || (r < 0 && errno == ECHILD)) {
        dead = true;
        reaped = true;
    }
    // Signal 3: control-socket EOF, corroborating — covers a child that dies
    // between heartbeat ticks or that waitpid has not yet surfaced.
    if (!dead && control_peer_closed(control_fd_)) {
        dead = true;
    }
    if (!dead) {
        return false;
    }
    if (reaped) {
        pid_ = -1; // zombie collected; restart/teardown must not re-reap it
    }
    // Latch crashed: drives the badge + restart offer. The audio thread's
    // CAS never leaves kCrashed, so this wins over an in-flight bypass flip.
    state_.store(LiveState::kCrashed, std::memory_order_release);
    return true;
}

// ── Restart: swap the child, keep everything else (§C.3) ─────────────
bool BridgedPlugin::restart(std::string& error) {
    // Session thread. The binding + its graph node are STABLE: only the
    // child process swaps. The host shm mapping is REUSED (never remapped),
    // so the graph binding, bundle, and reclamation fence are untouched, and
    // the audio thread — which does not touch shm while kCrashed — keeps
    // calling this same binding across the whole sequence.

    // 1) Reap the corpse if the reaper left one (SIGKILL is harmless on an
    //    already-dead pid; waitpid clears the zombie).
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    close_fd(control_fd_); // drop the dead child's control socket

    // The editor container, if still up, is stale: the child that owned the
    // reparented foreign window is dead, so X has already destroyed that
    // window. Drop the host container (destroys only our own window). Normally
    // update_editor() has already done this on the kCrashed transition; this
    // covers a restart driven without an intervening editor pump.
    editor_container_.reset();
    editor_child_window_ = 0;

    // 2) Sanitise + reset the reused segment in place. Uncontended: the
    //    audio thread is quiescent on shm while kCrashed.
    reset_control_block();

    // 3) Reset the audio-thread pipeline locals (also quiescent while
    //    kCrashed) so the fresh child's first heartbeat is not misread as
    //    "still stale" and the input pipeline starts clean. The release
    //    store of kLive at step 6 publishes these before the audio thread
    //    resumes (its acquire-load in process_block pairs).
    last_heartbeat_ = 0;
    stale_blocks_ = 0;
    seq_ = 0;
    input_slot_ = nullptr;
    input_slot_open_ = false;
    pending_reset_ = false;

    // 4) Respawn against the same segment + a fresh control socket.
    if (!spawn_child(error)) {
        return false; // stays kCrashed; caller may retry
    }

    // 5) Reload the last-known-good shadow into the fresh child (§C.4). A
    //    first-ever restart with no shadow starts at plugin default; a load
    //    failure is non-fatal (the child is up either way).
    if (!shadow_state_.empty()) {
        control_load(shadow_state_);
    }
    notify_active();

    // 6) Publish live LAST (release): the reset segment + fresh child are
    //    fully visible before the audio thread resumes touching shm.
    state_.store(LiveState::kLive, std::memory_order_release);
    return true;
}

// Re-initialise the reused control block for a fresh child. Only atomic
// index/word stores and header scalars the audio thread never reads on the
// hot path — no non-atomic slot payload is touched (a slot is read only
// after its producer commits fresh contents), so this races with nothing.
void BridgedPlugin::reset_control_block() {
    ControlBlock& cb = *shm_.control();
    // Header, version-first — re-written exactly as BridgeShm::create so the
    // fresh child validates a pristine segment even if the untrusted dead
    // child scribbled it. abi_version is published LAST (release).
    cb.magic = kBridgeMagic;
    cb.block_frames = kBridgeBlockFrames;
    cb.sample_rate = config_.sample_rate;
    cb.in_channels = kBridgeChannels;
    cb.out_channels = kBridgeChannels;
    cb.struct_size = static_cast<std::uint32_t>(sizeof(ControlBlock));
    cb.child_ready.store(0, std::memory_order_relaxed);
    cb.child_status.store(0, std::memory_order_relaxed);
    cb.shutdown.store(0, std::memory_order_relaxed);
    cb.child_parked.store(0, std::memory_order_relaxed);
    cb.child_heartbeat.store(0, std::memory_order_relaxed);
    cb.control_epoch.fetch_add(1, std::memory_order_relaxed); // restart generation
    cb.in_write.store(0, std::memory_order_relaxed);
    cb.in_read.store(0, std::memory_order_relaxed);
    cb.out_write.store(0, std::memory_order_relaxed);
    cb.out_read.store(0, std::memory_order_relaxed);
    cb.abi_version.store(kBridgeAbiVersion, std::memory_order_release);
}

#else // !__linux__ — the whole bridge is a no-op on non-Linux (§H.2).

std::unique_ptr<BridgedPlugin> BridgedPlugin::spawn(const Config& /*config*/, std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return nullptr;
}

bool BridgedPlugin::spawn_child(std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return false;
}

void BridgedPlugin::teardown() noexcept {}

bool BridgedPlugin::running() const {
    return false;
}

bool BridgedPlugin::poll_liveness() {
    return false;
}

bool BridgedPlugin::restart(std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return false;
}

void BridgedPlugin::reset_control_block() {}

#endif // __linux__

BridgedPlugin::BridgedPlugin(BridgeShm shm, const Config& config)
    : shm_(std::move(shm)), config_(config), has_audio_input_(config.has_audio_input) {}

BridgedPlugin::~BridgedPlugin() {
    teardown();
}

// Acquires the block's input slot on first use and zeroes its event
// counts, so the runner's note/param calls append into fresh storage;
// returns the same slot until process_block commits it. Null means the
// ring is full (child behind) — callers drop the event/block (§A.5).
// Wait-free: `in_ring_acquire` returns the producer's own slot without
// advancing an index, so repeated calls within a block are idempotent.
InSlot* BridgedPlugin::open_input_slot() {
    if (input_slot_open_) {
        return input_slot_;
    }
    // Crashed: do not touch shm — restart() may be re-initialising it. The
    // caller (an event method or the input-publish) then drops the event,
    // matching the ring-full policy (§A.5).
    if (state_.load(std::memory_order_acquire) == LiveState::kCrashed) {
        return nullptr;
    }
    input_slot_ = in_ring_acquire(*shm_.control());
    if (input_slot_ != nullptr) {
        input_slot_->note_count = 0;
        input_slot_->param_count = 0;
        input_slot_open_ = true;
    }
    return input_slot_;
}

// Bypass output stage (§C.1, wait-free): an effect passes its dry input
// through — a crashed insert must not punch a hole in the track; an
// instrument has no input to pass, so it goes silent. One memcpy or one
// fill, and it never touches the child.
void BridgedPlugin::write_bypass(const float* in, float* out, std::uint32_t nsamp) const {
    if (has_audio_input_ && in != nullptr) {
        std::memcpy(out, in, static_cast<std::size_t>(nsamp) * sizeof(float));
    } else {
        std::fill_n(out, nsamp, 0.0F);
    }
}

// ── The wait-free host callback path (§A.1) ──────────────────────────
// Every statement below is one of: an atomic load/store on the mlocked
// segment, a bounded memcpy/fill, or an integer op. There is no
// allocation, no syscall, no lock, and no unbounded loop — under any child
// state (fresh / slow / hung / dead / crashed) the worst case is one block
// of bypass output. This is the entire RT-safety argument, made
// mechanical; the alloc-free property is asserted under an RtScope in the
// tests. The added S29c liveness logic is a single lock-free counter
// compare (heartbeat staleness) plus an atomic CAS to publish the badge
// state — still wait-free.
void BridgedPlugin::process_block(const float* in, float* out, std::uint32_t frames) {
    const std::uint32_t nsamp = frames * kBridgeChannels;
    const LiveState state = state_.load(std::memory_order_acquire);

    // Reaper-confirmed death (§C): the child is gone and restart() may be
    // re-initialising the segment on the session thread. Do NOT touch shm —
    // emit bypass output and return. This gate is what keeps restart's
    // in-place reset uncontended (the audio thread reads shm only in
    // kLive/kBypassed). Drop any input slot the event calls left half-open
    // (never committed, so the ring write index is untouched).
    if (state == LiveState::kCrashed) {
        input_slot_ = nullptr;
        input_slot_open_ = false;
        pending_reset_ = false;
        write_bypass(in, out, nsamp);
        return;
    }

    ControlBlock& cb = *shm_.control();

    // ── Heartbeat-staleness liveness (§B.4 signal 1) ─────────────────
    // The audio thread's ONLY liveness logic: a wait-free counter compare.
    // The child bumps child_heartbeat every processed block; if it has not
    // advanced for kBypassStaleBlocks blocks the child is not producing, so
    // switch the OUTPUT to bypass. Input keeps being published below, so a
    // merely-slow child that catches up recovers on its own (kBypassed ->
    // kLive) — bypass does NOT latch here; only the reaper's kCrashed does.
    const std::uint64_t hb = cb.child_heartbeat.load(std::memory_order_acquire);
    if (hb != last_heartbeat_) {
        last_heartbeat_ = hb;
        stale_blocks_ = 0;
    } else if (stale_blocks_ < kBypassStaleBlocks) {
        ++stale_blocks_;
    }
    const bool bypassing = stale_blocks_ >= kBypassStaleBlocks;

    // Publish the kLive<->kBypassed transition for the badge via a CAS that
    // refuses to leave kCrashed (a reaper-confirmed death always wins).
    if (bypassing && state == LiveState::kLive) {
        LiveState expected = LiveState::kLive;
        state_.compare_exchange_strong(expected, LiveState::kBypassed, std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
    } else if (!bypassing && state == LiveState::kBypassed) {
        LiveState expected = LiveState::kBypassed;
        state_.compare_exchange_strong(expected, LiveState::kLive, std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
    }

    // 1) Publish this block's input. The note/param events staged by the
    //    runner's plugin_note_on/off + plugin_set_param_cv calls already
    //    live in this slot (open_input_slot acquired it); fill the audio and
    //    header and commit. A full ring means the child is behind: drop the
    //    whole block (§A.5). Published even while bypassing, so a slow child
    //    has fresh work to recover on. Transport stays zeroed — a wire field
    //    for future use (in-process ClapPlugin runs transport=nullptr).
    if (InSlot* slot = open_input_slot()) {
        slot->seq = seq_++;
        slot->frames = frames;
        slot->reset = pending_reset_ ? 1U : 0U;
        slot->transport = Transport{};
        if (in != nullptr) {
            std::memcpy(slot->audio_in.data(), in, static_cast<std::size_t>(nsamp) * sizeof(float));
        } else {
            std::fill_n(slot->audio_in.data(), nsamp, 0.0F); // instrument: no dry input
        }
        in_ring_commit(cb);
        input_slot_ = nullptr;
        input_slot_open_ = false;
        pending_reset_ = false;
    } else {
        dropped_input_blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    // 2) Output. Bypass (kind-split) when the heartbeat is stale; otherwise
    //    consume the oldest ready output (the input from one block earlier),
    //    or silence when none is ready. The child's frame count is clamped
    //    since the child is untrusted (§A.4).
    if (bypassing) {
        write_bypass(in, out, nsamp);
    } else if (const OutSlot* slot = out_ring_peek(cb)) {
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

// ── Event marshalling into the input slot (audio thread, wait-free) ──
// Each call appends one fixed-size entry to the current block's slot and
// bumps a count; process_block publishes them with the block. All notes
// land at frame 0 — block-head quantisation, matching the in-process
// runner, whose bindings carry no sub-block offset (graph_runner.cpp:386).
// Overflow past the slot's fixed capacity drops the event and counts it;
// never grows storage, never UB.
void BridgedPlugin::plugin_note_on(int note, float velocity) {
    InSlot* slot = open_input_slot();
    if (slot == nullptr || slot->note_count >= kBridgeMaxNotesPerBlock) {
        dropped_notes_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    NoteEvent& event = slot->notes[slot->note_count++];
    event.frame = 0;
    event.on = 1;
    event.key = note;
    event.velocity = velocity;
}

void BridgedPlugin::plugin_note_off(int note) {
    InSlot* slot = open_input_slot();
    if (slot == nullptr || slot->note_count >= kBridgeMaxNotesPerBlock) {
        dropped_notes_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    NoteEvent& event = slot->notes[slot->note_count++];
    event.frame = 0;
    event.on = 0;
    event.key = note;
    event.velocity = 0.0F;
}

void BridgedPlugin::plugin_set_param_cv(int param_index, float value01) {
    InSlot* slot = open_input_slot();
    if (slot == nullptr || param_index < 0 || slot->param_count >= kBridgeMaxParamsPerBlock) {
        dropped_params_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ParamChange& change = slot->params[slot->param_count++];
    change.index = static_cast<std::uint32_t>(param_index);
    change.value01 = std::clamp(value01, 0.0F, 1.0F);
}

void BridgedPlugin::plugin_reset() {
    // Folded into the next published block (process_block sets slot->reset);
    // held across a dropped block so the reset is never lost.
    pending_reset_ = true;
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

std::uint64_t BridgedPlugin::dropped_notes() const {
    return dropped_notes_.load(std::memory_order_relaxed);
}

std::uint64_t BridgedPlugin::dropped_params() const {
    return dropped_params_.load(std::memory_order_relaxed);
}

std::uint64_t BridgedPlugin::dropped_input_blocks() const {
    return dropped_input_blocks_.load(std::memory_order_relaxed);
}

// ── Plugin state proxy over the control socket (§C) ──────────────────
// Session-thread, blocking request/reply — never the audio thread. The
// socket is reliable and ordered, so one request is answered by exactly
// one reply; a dead child surfaces as a read/write failure (false), never
// a hang. The child serialises these against its RT thread.
//
// control_save/control_load do the raw exchange; they mutate the CHILD's
// state, not `this`, so clang-tidy would offer to const them — kept
// non-const as they are I/O commands, matching the intent.
// NOLINTNEXTLINE(readability-make-member-function-const) — child-state I/O
bool BridgedPlugin::control_save(std::vector<std::uint8_t>& out) {
#if defined(__linux__)
    if (control_fd_ < 0) {
        return false; // echo mode: no plugin, no state
    }
    const ControlHeader request{.magic = kControlMagic,
                                .type = static_cast<std::uint32_t>(ControlMsg::kSaveRequest),
                                .length = 0,
                                .status = 0};
    if (!control_write_full(control_fd_, &request, sizeof(request))) {
        return false;
    }
    ControlHeader reply{};
    if (!control_read_full(control_fd_, &reply, sizeof(reply))) {
        return false;
    }
    if (reply.magic != kControlMagic ||
        reply.type != static_cast<std::uint32_t>(ControlMsg::kSaveReply) || reply.status != 0 ||
        reply.length > kControlMaxPayload) {
        return false;
    }
    std::vector<std::uint8_t> blob(reply.length);
    if (reply.length > 0 && !control_read_full(control_fd_, blob.data(), blob.size())) {
        return false;
    }
    out = std::move(blob);
    return true;
#else
    (void)out;
    return false;
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const) — mutating command
bool BridgedPlugin::control_load(const std::vector<std::uint8_t>& blob) {
#if defined(__linux__)
    if (control_fd_ < 0 || blob.size() > kControlMaxPayload) {
        return false;
    }
    const ControlHeader request{.magic = kControlMagic,
                                .type = static_cast<std::uint32_t>(ControlMsg::kLoadRequest),
                                .length = static_cast<std::uint32_t>(blob.size()),
                                .status = 0};
    if (!control_write_full(control_fd_, &request, sizeof(request))) {
        return false;
    }
    if (!blob.empty() && !control_write_full(control_fd_, blob.data(), blob.size())) {
        return false;
    }
    ControlHeader reply{};
    if (!control_read_full(control_fd_, &reply, sizeof(reply))) {
        return false;
    }
    return reply.magic == kControlMagic &&
           reply.type == static_cast<std::uint32_t>(ControlMsg::kLoadReply) && reply.status == 0;
#else
    (void)blob;
    return false;
#endif
}

// Refresh the shadow from the live child (§C.4). No-op with a dead child —
// the existing shadow is then the last-known-good.
bool BridgedPlugin::capture_shadow() {
    if (state_.load(std::memory_order_acquire) == LiveState::kCrashed) {
        return false;
    }
    std::vector<std::uint8_t> blob;
    if (!control_save(blob)) {
        return false;
    }
    shadow_state_ = std::move(blob);
    return true;
}

// Refresh from the live child when possible, then hand back the shadow.
// With a dead child capture_shadow no-ops and the last-known-good shadow is
// returned — so the session stays saveable without a hang (§C.4).
std::vector<std::uint8_t> BridgedPlugin::save_state() {
    capture_shadow();
    return shadow_state_;
}

// Apply to the child and adopt as the shadow (§C.4 "at load").
bool BridgedPlugin::load_state(const std::vector<std::uint8_t>& blob) {
    if (!control_load(blob)) {
        return false;
    }
    shadow_state_ = blob;
    return true;
}

// ── Cross-process editor exchanges over the control socket (§D) ──────
// Session-thread, blocking request/reply — the same lockstep the state ops
// use. control_open_editor fills `info` with the child's editor window id +
// geometry; both mutate the CHILD (open/destroy its editor), not `this`, so
// clang-tidy would offer to const them — kept non-const as I/O commands,
// matching control_save/control_load.
// NOLINTNEXTLINE(readability-make-member-function-const) — child-state I/O
bool BridgedPlugin::control_open_editor(EditorWindowInfo& info) {
#if defined(__linux__)
    if (control_fd_ < 0) {
        return false; // echo mode: no plugin, no editor
    }
    const ControlHeader request{.magic = kControlMagic,
                                .type = static_cast<std::uint32_t>(ControlMsg::kOpenEditorRequest),
                                .length = 0,
                                .status = 0};
    if (!control_write_full(control_fd_, &request, sizeof(request))) {
        return false;
    }
    ControlHeader reply{};
    if (!control_read_full(control_fd_, &reply, sizeof(reply))) {
        return false;
    }
    if (reply.magic != kControlMagic ||
        reply.type != static_cast<std::uint32_t>(ControlMsg::kOpenEditorReply) ||
        reply.status != 0 || reply.length != sizeof(EditorWindowInfo)) {
        return false; // child has no gui / no display, or a desync
    }
    return control_read_full(control_fd_, &info, sizeof(info));
#else
    (void)info;
    return false;
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const) — child-state I/O
bool BridgedPlugin::control_close_editor() {
#if defined(__linux__)
    if (control_fd_ < 0) {
        return false;
    }
    const ControlHeader request{.magic = kControlMagic,
                                .type = static_cast<std::uint32_t>(ControlMsg::kCloseEditorRequest),
                                .length = 0,
                                .status = 0};
    if (!control_write_full(control_fd_, &request, sizeof(request))) {
        return false;
    }
    ControlHeader reply{};
    if (!control_read_full(control_fd_, &reply, sizeof(reply))) {
        return false;
    }
    return reply.magic == kControlMagic &&
           reply.type == static_cast<std::uint32_t>(ControlMsg::kCloseEditorReply);
#else
    return false;
#endif
}

// ── Cross-process editor (§D, session thread) ────────────────────────
// The child owns the plugin editor on its own UI thread and X connection;
// the host owns a container it reparents that foreign window into. The
// cross-process reparent works because X11 window ids are display-global,
// and the foreign-window lifetime is made safe by (1) the reaper-driven
// teardown in update_editor() and (2) the guarded X error handler installed
// when the container adopts the foreign window (editor_host_surface_x11.cpp).
bool BridgedPlugin::open_editor(std::string& error) {
    if (editor_container_ != nullptr) {
        return true; // already open
    }
    if (state_.load(std::memory_order_acquire) != LiveState::kLive) {
        error = "bridged plugin is not live";
        return false;
    }
    EditorWindowInfo info{};
    if (!control_open_editor(info)) {
        error = "the bridge child could not open the plugin editor";
        return false;
    }
    // Host-owned container, framed with the WM decorations and sized to the
    // child's editor; the child's editor window is a foreign, cross-process
    // X11 window reparented into it.
    const std::string title = config_.plugin_id.empty() ? "Plugin Editor" : config_.plugin_id;
    auto container =
        ext::EditorHostSurface::open(title, info.width, info.height, info.resizable != 0, error);
    if (container == nullptr) {
        control_close_editor(); // undo the child-side editor (e.g. no host display)
        return false;           // error set by open()
    }
    if (!container->adopt_foreign_child(static_cast<std::uintptr_t>(info.window_id))) {
        error = "failed to reparent the plugin editor window (child gone?)";
        control_close_editor();
        return false; // container destroyed as it leaves scope
    }
    editor_child_window_ = static_cast<std::uintptr_t>(info.window_id);
    editor_container_ = std::move(container);
    return true;
}

void BridgedPlugin::update_editor() {
    if (editor_container_ == nullptr) {
        return;
    }
    // Reaper-confirmed death (§D.2): X destroyed the foreign editor window when
    // the child's connection dropped. Tear down the host container — our own
    // window only; the foreign window is already gone, so we never touch it.
    // This is the crash-while-open teardown, and it cannot fault or hang.
    if (state_.load(std::memory_order_acquire) == LiveState::kCrashed) {
        editor_container_.reset();
        editor_child_window_ = 0;
        return;
    }
    // Pump the container's own events. It is host-owned, so this is safe even
    // if the child just died: a racing foreign-window request is swallowed by
    // the guarded X error handler installed on adopt.
    if (!editor_container_->pump()) {
        close_editor(); // user hit the container's WM close button
        return;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (editor_container_->take_resize(width, height)) {
        editor_container_->resize_foreign_child(width, height); // relay to the embedded window
    }
}

void BridgedPlugin::close_editor() {
    if (editor_container_ == nullptr) {
        return;
    }
    // Tell the child to destroy its editor (gui before window) BEFORE we drop
    // the container, so the foreign window is gone first and we never destroy a
    // window we do not own. A dead child makes this a harmless no-op.
    control_close_editor();
    editor_container_.reset();
    editor_child_window_ = 0;
}

} // namespace nt::ext::bridge
