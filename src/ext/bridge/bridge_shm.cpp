#include "ext/bridge/bridge_shm.h"

#include <new>
#include <utility>

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#endif // __linux__

namespace nt::ext::bridge {

#if defined(__linux__)

namespace {

constexpr std::uint64_t kSegmentSize = sizeof(ControlBlock);

// The futex syscall operates on a raw 32-bit word. std::atomic<uint32_t>
// is lock-free here (asserted in bridge_protocol.h) and therefore
// layout-compatible with a plain uint32_t, so its address is a valid
// futex address — the same reinterpretation libstdc++'s own atomic_wait
// performs internally. FUTEX_WAIT/WAKE are used *without* the private
// flag so the wake crosses the process boundary (futex(2)).
std::uint32_t* futex_word(std::atomic<std::uint32_t>& word) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — atomic<u32> is a raw u32 word
    return reinterpret_cast<std::uint32_t*>(&word);
}

long futex_call(std::uint32_t* addr, int op, std::uint32_t val, const timespec* timeout) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) — syscall is the only futex entry point
    return ::syscall(SYS_futex, addr, op, val, timeout, nullptr, 0);
}

} // namespace

BridgeShm::~BridgeShm() {
    reset();
}

BridgeShm::BridgeShm(BridgeShm&& other) noexcept
    : control_(other.control_), base_(other.base_), size_(other.size_), fd_(other.fd_),
      owner_(other.owner_), name_(std::move(other.name_)) {
    other.control_ = nullptr;
    other.base_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
    other.owner_ = false;
}

BridgeShm& BridgeShm::operator=(BridgeShm&& other) noexcept {
    if (this != &other) {
        reset();
        control_ = other.control_;
        base_ = other.base_;
        size_ = other.size_;
        fd_ = other.fd_;
        owner_ = other.owner_;
        name_ = std::move(other.name_);
        other.control_ = nullptr;
        other.base_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
        other.owner_ = false;
    }
    return *this;
}

void BridgeShm::reset() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (owner_ && !name_.empty()) {
        ::shm_unlink(name_.c_str());
    }
    control_ = nullptr;
    base_ = nullptr;
    size_ = 0;
    fd_ = -1;
    owner_ = false;
    name_.clear();
}

BridgeShm BridgeShm::create(const std::string& name, std::uint32_t sample_rate,
                            std::uint32_t in_channels, std::uint32_t out_channels,
                            std::string& error) {
    BridgeShm shm;
    // O_EXCL: the host owns a freshly named segment; colliding with a
    // stale one is a bug, not something to silently adopt.
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        error = "shm_open(create) failed: " + std::string(std::strerror(errno));
        return shm;
    }
    if (::ftruncate(fd, static_cast<off_t>(kSegmentSize)) != 0) {
        error = "ftruncate failed: " + std::string(std::strerror(errno));
        ::close(fd);
        ::shm_unlink(name.c_str());
        return shm;
    }
    void* base = ::mmap(nullptr, kSegmentSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        error = "mmap failed: " + std::string(std::strerror(errno));
        ::close(fd);
        ::shm_unlink(name.c_str());
        return shm;
    }
    // Best-effort lock: keep the RT path off the pager. A denied lock
    // (RLIMIT_MEMLOCK) is not fatal — the one-block buffer absorbs a
    // stray fault, and raising the limit is documented power-user
    // tuning, never a precondition (§H.3).
    ::mlock(base, kSegmentSize);

    // Construct the control block once, on the creating side, over the
    // zero-filled mapping. Placement-new begins the object's lifetime;
    // the child observes the same bytes and reinterprets them (attach()).
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — placement new; munmap frees it
    auto* cb = ::new (base) ControlBlock{};
    cb->magic = kBridgeMagic;
    cb->block_frames = kBridgeBlockFrames;
    cb->sample_rate = sample_rate;
    cb->in_channels = in_channels;
    cb->out_channels = out_channels;
    cb->struct_size = static_cast<std::uint32_t>(kSegmentSize);
    // abi_version is published LAST (release) so a child that observes it
    // set is guaranteed to see the fully written header behind it.
    cb->abi_version.store(kBridgeAbiVersion, std::memory_order_release);

    shm.control_ = cb;
    shm.base_ = base;
    shm.size_ = kSegmentSize;
    shm.fd_ = fd;
    shm.owner_ = true;
    shm.name_ = name;
    return shm;
}

BridgeShm BridgeShm::attach(const std::string& name, std::string& error) {
    BridgeShm shm;
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        error = "shm_open(attach) failed: " + std::string(std::strerror(errno));
        return shm;
    }
    void* base = ::mmap(nullptr, kSegmentSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        error = "mmap failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return shm;
    }
    ::mlock(base, kSegmentSize);

    // The host already constructed the ControlBlock; the child adopts the
    // mapped bytes as that object. Formally the object's lifetime began
    // in the host process (std::start_lifetime_as is the C++23 blessing);
    // this reinterpret is the accepted cross-process shm idiom.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — shm object adoption
    auto* cb = reinterpret_cast<ControlBlock*>(base);

    // Version-first handshake (§B.3): abi_version is read (acquire, to
    // pair with the host's release) and validated before ANY other field.
    const std::uint32_t version = cb->abi_version.load(std::memory_order_acquire);
    if (version != kBridgeAbiVersion) {
        error = "abi_version mismatch: segment " + std::to_string(version) + ", child " +
                std::to_string(kBridgeAbiVersion);
        ::munmap(base, kSegmentSize);
        ::close(fd);
        return shm;
    }
    if (cb->magic != kBridgeMagic || cb->block_frames != kBridgeBlockFrames ||
        cb->struct_size != static_cast<std::uint32_t>(kSegmentSize)) {
        error = "bridge segment header mismatch (magic/block_frames/struct_size)";
        ::munmap(base, kSegmentSize);
        ::close(fd);
        return shm;
    }

    shm.control_ = cb;
    shm.base_ = base;
    shm.size_ = kSegmentSize;
    shm.fd_ = fd;
    shm.owner_ = false; // child never unlinks
    shm.name_ = name;
    return shm;
}

void bridge_futex_wake(std::atomic<std::uint32_t>& word) {
    futex_call(futex_word(word), FUTEX_WAKE, /*val=*/0x7fffffff, nullptr);
}

void bridge_futex_wait(std::atomic<std::uint32_t>& word, std::uint32_t expected,
                       std::int64_t timeout_ns) {
    const timespec timeout{.tv_sec = static_cast<time_t>(timeout_ns / 1'000'000'000),
                           .tv_nsec = static_cast<long>(timeout_ns % 1'000'000'000)};
    futex_call(futex_word(word), FUTEX_WAIT, expected, &timeout);
}

#else // !__linux__ — documented unsupported stub (Windows bridge: §H.2)

BridgeShm::~BridgeShm() = default;

BridgeShm::BridgeShm(BridgeShm&&) noexcept = default;

BridgeShm& BridgeShm::operator=(BridgeShm&&) noexcept = default;

void BridgeShm::reset() noexcept {}

BridgeShm BridgeShm::create(const std::string& /*name*/, std::uint32_t /*sample_rate*/,
                            std::uint32_t /*in_channels*/, std::uint32_t /*out_channels*/,
                            std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return {};
}

BridgeShm BridgeShm::attach(const std::string& /*name*/, std::string& error) {
    error = "out-of-process plugin bridge is unsupported on this platform (Linux-first, §H.2)";
    return {};
}

void bridge_futex_wake(std::atomic<std::uint32_t>& /*word*/) {}

void bridge_futex_wait(std::atomic<std::uint32_t>& /*word*/, std::uint32_t /*expected*/,
                       std::int64_t /*timeout_ns*/) {}

#endif // __linux__

} // namespace nt::ext::bridge
