// api/local_api — the Local API: a localhost WebSocket server exposing
// the session's command surface to external tools (editor scripts,
// LLM agents, companion apps). Behavioural reference: the web app's
// lib/trackerLocalApi.ts + trackerLocalApiSchema.ts relay protocol —
// frame vocabulary, command names and reply shapes are kept verbatim
// so existing client scripts keep working against the native port.
//
// Wire protocol (web RelayFrame shape, trackerLocalApiTypes.ts):
//   → {"type":"hello","token":"…"}                 first frame, auth
//   ← {"type":"welcome","role":"active","version":…}
//   → {"type":"request","requestId":…,"kind":"execute","commands":[…],
//      "opts":{"undoDescription":…,"dryRun":…}}
//   → {"type":"request","requestId":…,"kind":"read","query":{"op":…}}
//   ← {"type":"reply","requestId":…,"result":{BatchResult|QueryResult}}
//   → {"type":"ping"}  ← {"type":"pong"}
//   ← {"type":"error","code":…,"message":…}        protocol failures
// Auth is a first-message handshake: anything before a valid hello is
// rejected with code "unauthorized" and the connection closes. The
// server binds 127.0.0.1 only; the bearer token (settings) is the
// trust boundary on the local machine.
//
// Fix-don't-retain: web fix-list #5 is paid here — sample binary
// upload lands (loadSampleData, base64 → the standard decode path),
// workspace-ID discovery exists (getWorkspace enumerates nodes, ports
// and cables), and every id (pattern, sample slot, workspace node,
// port, cable, param) is validated before touching the session; bogus
// ids come back as typed errors, never silently accepted.
//
// Threading contract: IXWebSocket runs one accept thread plus one
// worker thread per connection. Those threads only parse JSON,
// validate frame shape/size, and enqueue into a bounded queue — they
// never touch ProjectSession, AudioEngine state, or any project data.
// process_pending() is the single consumer: called once per frame on
// the UI thread (the test thread in the loopback suite), it drains
// the queue, executes every request against the session, and sends
// replies (ix::WebSocket::send is thread-safe, and the locked
// shared_ptr keeps a departing socket alive for the send). Client
// records and the request log are guarded by an internal mutex so the
// window and the server threads can both touch them; no session state
// ever crosses that mutex.
#pragma once

#include "app/project_session.h"
#include "audio/audio_engine.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ix {
class WebSocketServer;
class WebSocket;
} // namespace ix

namespace nt::api {

// Protocol version reported by welcome/getSchema. The web surface was
// "1.1"; the native additions (transport/project/workspace/plugin ops,
// sample upload, discovery queries) bump the minor.
inline constexpr const char* kLocalApiVersion = "1.2";

// Wire and queue bounds. The message cap is far above the web's 1 MiB
// batch limit so base64 sample uploads fit (divergence logged in
// FIXES.md); the queue bounds keep a flooding client from outrunning
// the frame-loop drain — overflow is rejected immediately with
// "rateLimited", never buffered unboundedly.
inline constexpr std::size_t kMaxMessageBytes = 64ULL * 1024 * 1024;
inline constexpr std::size_t kMaxQueuedRequests = 64;
inline constexpr std::size_t kMaxQueuedBytes = 128ULL * 1024 * 1024;
inline constexpr int kMaxCommandsPerBatch = 10000;

// One connected client, as shown in the LOCAL API window.
struct ClientInfo {
    std::string id;      // IXWebSocket connection id
    std::string address; // remote ip:port (loopback by construction)
    std::chrono::system_clock::time_point connected_at;
    std::uint64_t requests = 0;
    bool authed = false;
};

// One request-log line (bounded ring; the window shows the tail).
// `kind` matches the web activity log: execute | read | denied | error.
struct LogEntry {
    std::chrono::system_clock::time_point time;
    std::string kind;
    std::string description;
    bool ok = false;
};

// Crypto-random 128-bit bearer token as 32 hex chars. std::random_device
// is the entropy source — on every supported platform it is backed by
// the OS CSPRNG (std::mt19937 would not be acceptable here).
[[nodiscard]] std::string generate_token();

class LocalApiServer {
public:
    LocalApiServer();
    ~LocalApiServer(); // stops the server; joins IXWebSocket threads

    LocalApiServer(const LocalApiServer&) = delete;
    LocalApiServer& operator=(const LocalApiServer&) = delete;
    LocalApiServer(LocalApiServer&&) = delete;
    LocalApiServer& operator=(LocalApiServer&&) = delete;

    // Binds 127.0.0.1:`port` and starts listening. Returns false with
    // error() set (port in use, empty token). UI thread.
    bool start(int port, const std::string& token);
    void stop();

    [[nodiscard]] bool running() const { return running_; }

    [[nodiscard]] int port() const { return port_; }

    [[nodiscard]] const std::string& error() const { return error_; }

    // Drains queued requests against the session — UI thread, once per
    // frame (see the threading contract above). No-op when stopped.
    void process_pending(app::ProjectSession& session, audio::AudioEngine& audio);

    // Window surface: value copies under the mutex.
    [[nodiscard]] std::vector<ClientInfo> clients() const;
    [[nodiscard]] std::vector<LogEntry> log_tail() const;
    void clear_log();

private:
    struct PendingRequest {
        std::weak_ptr<ix::WebSocket> socket;
        std::string connection_id;
        std::string frame; // raw JSON text, parsed again on the UI thread
        std::size_t bytes = 0;
    };

    // Server-thread half: frame triage for one message (auth, ping,
    // enqueue). Defined in local_api.cpp.
    void on_client_message(const std::shared_ptr<ix::WebSocket>& socket,
                           const std::string& connection_id, const std::string& payload);

    void push_log(const std::string& kind, const std::string& description, bool ok);

    std::unique_ptr<ix::WebSocketServer> server_;
    bool running_ = false;
    int port_ = 0;
    std::string error_;

    mutable std::mutex mutex_; // guards everything below
    std::string token_;
    std::vector<ClientInfo> clients_;
    std::deque<PendingRequest> queue_;
    std::size_t queued_bytes_ = 0;
    std::deque<LogEntry> log_;
};

} // namespace nt::api
