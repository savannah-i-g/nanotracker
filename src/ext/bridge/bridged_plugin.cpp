#include "ext/bridge/bridged_plugin.h"

#include "ext/bridge/bridge_control_socket.h"
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

    // CLAP-hosting mode carries a plugin path/id and a control socket for
    // state; echo mode carries a gain and no socket (the pure-transport
    // S29a tests). Build the mode's argv and, when hosting, a socketpair
    // whose child end is dup'd onto the conventional control fd.
    const bool clap_mode = !config.plugin_path.empty();
    int host_fd = -1;  // host end of the control socket (kept; -1 in echo mode)
    int child_fd = -1; // child end (dup'd to kControlChildFd, then closed here)
    if (clap_mode) {
        std::array<int, 2> sv{-1, -1};
        // SOCK_CLOEXEC so neither end leaks into unrelated spawns; the
        // child end is re-materialised (non-CLOEXEC) via adddup2 below.
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv.data()) != 0) {
            error = "socketpair failed: " + std::string(std::strerror(errno));
            return nullptr; // shm destructor unlinks the segment
        }
        host_fd = sv[0];
        child_fd = sv[1];
    }

    // argv: echo = [exe, shm-name, "echo", gain]; clap = [exe, shm-name,
    // "clap", plugin-path, plugin-id]. Strings outlive posix_spawn.
    std::string exe_str = host_exe.string();
    std::string name_str = shm.name();
    std::string mode_str = clap_mode ? "clap" : "echo";
    std::string arg3 = clap_mode ? config.plugin_path.string() : std::to_string(config.echo_gain);
    std::string arg4 = config.plugin_id;
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
        return nullptr; // shm destructor unlinks the segment
    }
    close_fd(child_fd); // the child now owns its dup; the host keeps host_fd

    // Wait (off the audio thread, so blocking is fine) for the child to
    // create the plugin and signal ready, or for it to exit early on a
    // handshake / load failure. Bounded so a wedged child never hangs
    // startup, and a child that fails to load is seen as not-ready.
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
            error = "bridge-host child exited during handshake (plugin load failed?)";
            close_fd(host_fd);
            return nullptr; // reaped; shm destructor unlinks
        }
        sleep_ns(200'000); // 200 us
    }
    if (!ready) {
        error = "bridge-host child did not become ready";
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        close_fd(host_fd);
        return nullptr;
    }

    std::unique_ptr<BridgedPlugin> plugin(new BridgedPlugin(std::move(shm), pid, host_fd, config));
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

BridgedPlugin::BridgedPlugin(BridgeShm shm, int pid, int control_fd, const Config& config)
    : shm_(std::move(shm)), pid_(pid), control_fd_(control_fd),
      has_audio_input_(config.has_audio_input) {}

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
    input_slot_ = in_ring_acquire(*shm_.control());
    if (input_slot_ != nullptr) {
        input_slot_->note_count = 0;
        input_slot_->param_count = 0;
        input_slot_open_ = true;
    }
    return input_slot_;
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

    // 1) Publish this block's input. The note/param events staged by the
    //    runner's plugin_note_on/off + plugin_set_param_cv calls already
    //    live in this slot (open_input_slot acquired it); fill the audio
    //    and header and commit. A full ring means the child is behind:
    //    drop the whole block — events and audio — which the empty output
    //    ring already answers with silence (§A.5).
    //
    //    Transport is a wire field for future use: the graph runner does
    //    not thread transport to bindings and in-process ClapPlugin runs
    //    with transport=nullptr, so a zeroed Transport keeps the bridged
    //    process() call identical to in-process. Threading real transport
    //    is a later additive step — the field already exists, no layout
    //    or abi_version change.
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
// one reply; a dead child surfaces as a read/write failure (empty/false),
// never a hang. The child serialises these against its RT thread.
//
// Non-const by intent: save_state gains a last-known-good shadow-state
// member it refreshes here in S29c (§C.4), and load_state is a mutating
// command. NOLINTs keep that intent rather than tracking today's members.
// NOLINTNEXTLINE(readability-make-member-function-const)
std::vector<std::uint8_t> BridgedPlugin::save_state() {
#if defined(__linux__)
    if (control_fd_ < 0) {
        return {}; // echo mode: no plugin, no state
    }
    const ControlHeader request{.magic = kControlMagic,
                                .type = static_cast<std::uint32_t>(ControlMsg::kSaveRequest),
                                .length = 0,
                                .status = 0};
    if (!control_write_full(control_fd_, &request, sizeof(request))) {
        return {};
    }
    ControlHeader reply{};
    if (!control_read_full(control_fd_, &reply, sizeof(reply))) {
        return {};
    }
    if (reply.magic != kControlMagic ||
        reply.type != static_cast<std::uint32_t>(ControlMsg::kSaveReply) || reply.status != 0 ||
        reply.length > kControlMaxPayload) {
        return {};
    }
    std::vector<std::uint8_t> blob(reply.length);
    if (reply.length > 0 && !control_read_full(control_fd_, blob.data(), blob.size())) {
        return {};
    }
    return blob;
#else
    return {};
#endif
}

// NOLINTNEXTLINE(readability-make-member-function-const) — mutating command
bool BridgedPlugin::load_state(const std::vector<std::uint8_t>& blob) {
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

} // namespace nt::ext::bridge
