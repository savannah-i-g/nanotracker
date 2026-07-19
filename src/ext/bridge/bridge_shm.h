// ext/bridge/bridge_shm — the POSIX shared-memory + futex platform seam
// for the plugin bridge. Everything OS-specific about the transport
// lives behind this header: segment create/attach/detach and the
// cross-process wake primitive. Linux-first per owner decision §H.2;
// non-Linux builds compile as a documented "unsupported" stub so the
// rest of the tree still builds (the Windows bridge is a fast-follow).
//
// The ring layout and the wait-free ring operations live in
// bridge_protocol.h; this header only owns the mapping lifecycle. The
// mapped segment is mlocked (best-effort) so the audio thread never
// page-faults on it, the same precaution yabridge/Carla take (§A.4).
#pragma once

#include "ext/bridge/bridge_protocol.h"

#include <cstdint>
#include <string>

namespace nt::ext::bridge {

// Owns one shared-memory segment mapping and its lifetime. Move-only:
// the mapping is unmapped on destruction and, for the creating (host)
// side, the segment name is unlinked so a crash never leaks it beyond
// the two processes that share it.
class BridgeShm {
public:
    BridgeShm() = default;
    ~BridgeShm();

    BridgeShm(const BridgeShm&) = delete;
    BridgeShm& operator=(const BridgeShm&) = delete;
    BridgeShm(BridgeShm&& other) noexcept;
    BridgeShm& operator=(BridgeShm&& other) noexcept;

    // Host side: shm_open(O_CREAT|O_EXCL) + ftruncate + mmap + mlock,
    // then the version-first header (magic, abi_version, geometry) is
    // written and the rings zeroed. Returns an invalid segment with
    // `error` set on failure (or on any non-Linux platform).
    static BridgeShm create(const std::string& name, std::uint32_t sample_rate,
                            std::uint32_t in_channels, std::uint32_t out_channels,
                            std::string& error);

    // Child side: shm_open + mmap + mlock, then the header is validated
    // (abi_version checked FIRST, then magic / block_frames /
    // struct_size). Returns an invalid segment with `error` set on any
    // mismatch — the child refuses a stale or foreign segment.
    static BridgeShm attach(const std::string& name, std::string& error);

    [[nodiscard]] bool valid() const { return control_ != nullptr; }

    [[nodiscard]] ControlBlock* control() const { return control_; }

    [[nodiscard]] const std::string& name() const { return name_; }

private:
    void reset() noexcept;

    ControlBlock* control_ = nullptr;
    void* base_ = nullptr;
    std::uint64_t size_ = 0;
    int fd_ = -1;
    bool owner_ = false; // created here → unlink on destruction
    std::string name_;
};

// Cross-process wake for the child's idle park (§A.3). Both are safe to
// call while the peer is not parked. `bridge_futex_wake` is issued only
// off the audio thread (host session thread, on idle->active); the child
// alone ever waits. On non-Linux these are no-ops.
void bridge_futex_wake(std::atomic<std::uint32_t>& word);

// Waits until `word` differs from `expected` or `timeout_ns` elapses.
// Returns immediately if they already differ. Child RT thread only.
void bridge_futex_wait(std::atomic<std::uint32_t>& word, std::uint32_t expected,
                       std::int64_t timeout_ns);

} // namespace nt::ext::bridge
