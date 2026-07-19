// ext/bridge/bridge_control_socket — the host<->child control-channel
// protocol (design 01-plugin-bridge-design.md §B/§C).
//
// Alongside the wait-free shared-memory ring (bridge_protocol.h) the two
// processes share a Unix-domain socketpair. It carries everything that is
// variable-size or main-thread and therefore must stay OFF the RT path:
// S29b uses it for plugin state save/load (the mechanism S29c
// crash-restart and S29e persistence build on). The socket is reliable,
// ordered, and returns EOF when the peer dies — a crash signal S29c will
// also consume (§B.4).
//
// ── Wire shape ────────────────────────────────────────────────────────
// Every message is a fixed ControlHeader followed by `length` payload
// bytes. The channel runs in strict request/reply lockstep: the host
// sends one request and reads exactly one reply before the next, so the
// stream never interleaves and neither side needs framing beyond the
// length prefix. Only POD crosses the boundary; both ends ship together
// and share this header, so there is no cross-toolchain layout risk.
//
// ── Threading ─────────────────────────────────────────────────────────
// Host side: the session thread only (never the audio thread) — blocking
// I/O is legal there. Child side: the child's control thread, which
// serialises state ops against its RT thread with a mutex (§C). The
// read/write helpers loop past partial I/O and EINTR and report a dead
// peer as a clean failure, never a hang.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nt::ext::bridge {

// Control-channel identity. Distinct from the shm segment magic so a
// crossed wire is caught rather than misread.
inline constexpr std::uint32_t kControlMagic = 0x4E544353U; // "NTCS"

// Fixed conventional descriptor the child inherits for its control-socket
// end: the host dup2's the child socket onto this number in the spawn
// file actions, so the child opens a known fd without parsing argv for it.
inline constexpr int kControlChildFd = 3;

// Upper bound on a single payload (state blobs). A child is untrusted
// (§A.4): a header claiming more than this is treated as a desync and the
// channel is torn down rather than allocating an absurd buffer.
inline constexpr std::uint32_t kControlMaxPayload = 64U * 1024U * 1024U;

// Request/reply kinds. Each request is answered by exactly one reply of
// the matching kind; unknown kinds are a desync (the reader bails). The
// underlying type mirrors the 32-bit ControlHeader.type field it is cast
// into, so the wire width is explicit at the definition.
// NOLINTNEXTLINE(performance-enum-size) — width matches the wire field
enum class ControlMsg : std::uint32_t {
    kSaveRequest = 1U, // host -> child: no payload
    kSaveReply = 2U,   // child -> host: payload = opaque plugin state blob
    kLoadRequest = 3U, // host -> child: payload = opaque plugin state blob
    kLoadReply = 4U,   // child -> host: no payload; status = applied ? 0 : 1
};

struct ControlHeader {
    std::uint32_t magic = kControlMagic;
    std::uint32_t type = 0;   // ControlMsg
    std::uint32_t length = 0; // payload bytes following this header
    std::uint32_t status = 0; // reply status: 0 = ok, nonzero = error
};

#if defined(__linux__)

// Write exactly `len` bytes, looping past short writes and EINTR. Returns
// false on a hard error or a peer that has gone away — the caller then
// abandons the exchange (host: state op fails; child: control loop exits).
inline bool control_write_full(int fd, const void* buf, std::size_t len) {
    const auto* bytes = static_cast<const std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, bytes + off, len - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
        } else if (n == 0 || errno != EINTR) {
            return false; // no progress or hard error (EINTR alone retries)
        }
    }
    return true;
}

// Read exactly `len` bytes. Returns false on EOF (peer closed/died) or a
// hard error, so a dead child surfaces as a bounded failure, never a hang.
inline bool control_read_full(int fd, void* buf, std::size_t len) {
    auto* bytes = static_cast<std::uint8_t*>(buf);
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::read(fd, bytes + off, len - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
        } else if (n == 0 || errno != EINTR) {
            return false; // EOF (n == 0) or hard error (EINTR alone retries)
        }
    }
    return true;
}

// Corroborating crash signal (§B.4 signal 3): true once the peer has closed
// its end of the control socket — the child's death closes every fd it
// holds, so a dead child shows up here as POLLHUP/EOF. Non-blocking and
// non-destructive: it MSG_PEEKs at most one byte and never consumes reply
// data, so it is safe to poll between save/load exchanges on the session
// thread (the only thread that touches this socket). Never the audio
// thread. A still-alive child reads as "not closed".
inline bool control_peer_closed(int fd) {
    if (fd < 0) {
        return false;
    }
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    if (::poll(&pfd, 1, /*timeout_ms=*/0) <= 0) {
        return false; // timeout or error: nothing to conclude
    }
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return true;
    }
    if ((pfd.revents & POLLIN) != 0) {
        // Readable: either a reply (never pending here in lockstep) or EOF.
        // Peek without consuming; a zero-length peek is the closed peer.
        std::uint8_t byte = 0;
        const ssize_t n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        return n == 0;
    }
    return false;
}

#endif // __linux__

} // namespace nt::ext::bridge
