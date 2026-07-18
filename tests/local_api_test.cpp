// Local API loopback verification (Stage 18): the real WebSocket
// server on a localhost port, a real IXWebSocket client, and the test
// thread standing in for the UI thread — every await pumps
// process_pending, which is the session's single consumer exactly as
// in the app's frame loop. The engine runs offline (no device), so
// transport state is observed by pulling render blocks by hand.
#include "api/local_api.h"
#include "app/project_session.h"
#include "engine/tracker_engine.h"
#include "io/ftrk_writer.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using nlohmann::json;

constexpr const char* kToken = "feedfacefeedfacefeedfacefeedface";

// Minimal 16-bit mono WAV with a 440 Hz sine (in memory — the upload
// payload and the loopback decode reference).
std::vector<std::uint8_t> make_wav_bytes() {
    constexpr std::uint32_t kRate = 22050;
    constexpr std::uint32_t kFrames = 2205;
    std::vector<std::uint8_t> bytes;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back((v >> (8U * static_cast<unsigned>(i))) & 0xFFU);
        }
    };
    const auto u16 = [&](std::uint16_t v) {
        bytes.push_back(v & 0xFFU);
        bytes.push_back((v >> 8U) & 0xFFU);
    };
    const auto tag = [&](const char* t) { bytes.insert(bytes.end(), t, t + 4); };
    tag("RIFF");
    u32(36 + (kFrames * 2));
    tag("WAVE");
    tag("fmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(kRate);
    u32(kRate * 2);
    u16(2);
    u16(16);
    tag("data");
    u32(kFrames * 2);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        const auto s =
            static_cast<std::int16_t>(20000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / kRate));
        u16(static_cast<std::uint16_t>(s));
    }
    return bytes;
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    constexpr const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U) | bytes[i + 2];
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[v & 0x3FU]);
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16U;
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8U);
        out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

// Server + offline session + loopback client. Awaiting a frame pumps
// process_pending on this (the "UI") thread.
struct Loopback {
    nt::audio::AudioEngine audio;
    // Member order matters: offline mode (no device; the test thread
    // pulls blocks) must be live before the session publishes bundles.
    bool offline_started = audio.start_offline(48000);
    nt::app::ProjectSession session{audio};
    nt::api::LocalApiServer server;
    ix::WebSocket client;
    std::mutex mutex;
    std::vector<json> frames;
    int request_counter = 0;

    Loopback() {
        REQUIRE(offline_started);
        ix::initNetSystem();
        int port = -1;
        for (int candidate = 38931; candidate < 38995; ++candidate) {
            if (server.start(candidate, kToken)) {
                port = candidate;
                break;
            }
        }
        REQUIRE(port > 0);
        client.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/");
        client.disableAutomaticReconnection();
        client.disablePerMessageDeflate();
        client.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message) {
                const json parsed = json::parse(msg->str, nullptr, /*allow_exceptions=*/false);
                if (!parsed.is_discarded()) {
                    const std::lock_guard<std::mutex> lock(mutex);
                    frames.push_back(parsed);
                }
            }
        });
        client.start();
        REQUIRE(wait_for([this] { return client.getReadyState() == ix::ReadyState::Open; }));
    }

    ~Loopback() {
        client.stop();
        server.stop();
    }

    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;
    Loopback(Loopback&&) = delete;
    Loopback& operator=(Loopback&&) = delete;

    // The pump: the deadline (5 s) only bites on regressions; a
    // passing run leaves each wait after a few milliseconds.
    bool wait_for(const std::function<bool()>& pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            server.process_pending(session, audio);
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    json await_frame(const std::function<bool(const json&)>& pred) {
        json found;
        const bool ok = wait_for([&] {
            const std::lock_guard<std::mutex> lock(mutex);
            for (const json& frame : frames) {
                if (pred(frame)) {
                    found = frame;
                    return true;
                }
            }
            return false;
        });
        REQUIRE(ok);
        return found;
    }

    json hello(const std::string& token) {
        client.send(json{{"type", "hello"}, {"token", token}}.dump());
        return await_frame([](const json& f) {
            const std::string type = f.value("type", "");
            return type == "welcome" || type == "error";
        });
    }

    void authenticate() {
        const json welcome = hello(kToken);
        REQUIRE(welcome.value("type", "") == "welcome");
    }

    json request(const std::string& kind, const json& body) {
        const std::string id = "req-" + std::to_string(++request_counter);
        json frame{{"type", "request"}, {"requestId", id}, {"kind", kind}};
        frame.update(body);
        client.send(frame.dump());
        const json reply = await_frame([&id](const json& f) {
            return f.value("type", "") == "reply" && f.value("requestId", "") == id;
        });
        return reply.at("result");
    }

    json execute(const json& commands, const std::string& undo_description = "test batch") {
        return request("execute", json{{"commands", commands},
                                       {"opts", {{"undoDescription", undo_description}}}});
    }

    json read(const json& query) { return request("read", json{{"query", query}}); }

    // Offline engines have no device pull; draining a block applies
    // queued transport commands and republishes the snapshot.
    void drain_engine() {
        std::vector<float> block(static_cast<std::size_t>(256) * 2, 0.0F);
        audio.render_offline(block.data(), 256);
    }
};

// By value: callers pass temporary results, so a reference would
// dangle past the full expression.
json first_error(const json& result) {
    REQUIRE(result.at("ok").get<bool>() == false);
    REQUIRE(result.at("errors").is_array());
    REQUIRE(!result.at("errors").empty());
    return result.at("errors").at(0);
}

} // namespace

TEST_CASE("local api auth handshake", "[local_api]") {
    SECTION("bad token is rejected and the connection closes") {
        Loopback lb;
        const json reply = lb.hello("wrong-token");
        CHECK(reply.value("type", "") == "error");
        CHECK(reply.value("code", "") == "unauthorized");
        CHECK(lb.wait_for([&] { return lb.client.getReadyState() == ix::ReadyState::Closed; }));
    }

    SECTION("requests before hello are rejected and the connection closes") {
        Loopback lb;
        lb.client.send(json{{"type", "request"},
                            {"requestId", "r1"},
                            {"kind", "read"},
                            {"query", {{"op", "getProjectSummary"}}}}
                           .dump());
        const json reply =
            lb.await_frame([](const json& f) { return f.value("type", "") == "error"; });
        CHECK(reply.value("code", "") == "unauthorized");
        CHECK(lb.wait_for([&] { return lb.client.getReadyState() == ix::ReadyState::Closed; }));
    }

    SECTION("the right token is welcomed and requests flow") {
        Loopback lb;
        const json welcome = lb.hello(kToken);
        CHECK(welcome.value("type", "") == "welcome");
        CHECK(welcome.value("version", "") == nt::api::kLocalApiVersion);

        const json summary = lb.read(json{{"op", "getProjectSummary"}});
        REQUIRE(summary.at("ok").get<bool>());
        CHECK(summary.at("data").at("channels").get<int>() == 4);
    }
}

TEST_CASE("local api transport control", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    REQUIRE(lb.execute(json::array({{{"op", "play"}}})).at("ok").get<bool>());
    lb.drain_engine();
    CHECK(lb.session.playing());

    const json transport = lb.read(json{{"op", "getTransport"}});
    REQUIRE(transport.at("ok").get<bool>());
    CHECK(transport.at("data").at("playing").get<bool>());

    REQUIRE(lb.execute(json::array({{{"op", "stop"}}})).at("ok").get<bool>());
    lb.drain_engine();
    CHECK_FALSE(lb.session.playing());
}

TEST_CASE("local api cell writes and pattern reads", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    const json result =
        lb.execute(json::array({{{"op", "setCell"},
                                 {"patternId", 0},
                                 {"row", 0},
                                 {"channel", 0},
                                 {"cell", {{"note", 49}, {"instrument", 1}, {"volume", 64}}}}}));
    REQUIRE(result.at("ok").get<bool>());
    CHECK(result.at("commandsApplied").get<int>() == 1);

    const auto& cell = lb.session.project().patterns[0].rows[0][0];
    CHECK(cell.note == 49);
    CHECK(cell.instrument == 1);
    CHECK(cell.volume == 64);

    // Partial merge: writing only the volume keeps the note (web
    // setCell semantics).
    REQUIRE(lb.execute(json::array({{{"op", "setCell"},
                                     {"patternId", 0},
                                     {"row", 0},
                                     {"channel", 0},
                                     {"cell", {{"volume", 32}}}}}))
                .at("ok")
                .get<bool>());
    CHECK(lb.session.project().patterns[0].rows[0][0].note == 49);
    CHECK(lb.session.project().patterns[0].rows[0][0].volume == 32);

    // Round trip through getPattern sees the same cell.
    const json pattern = lb.read(json{{"op", "getPattern"}, {"patternId", 0}});
    REQUIRE(pattern.at("ok").get<bool>());
    const json& read_cell = pattern.at("data").at("rows").at(0).at(0);
    CHECK(read_cell.at("note").get<int>() == 49);
    CHECK(read_cell.at("volume").get<int>() == 32);

    // setNoteOff writes the canonical release cell.
    REQUIRE(lb.execute(json::array(
                           {{{"op", "setNoteOff"}, {"patternId", 0}, {"row", 4}, {"channel", 0}}}))
                .at("ok")
                .get<bool>());
    CHECK(lb.session.project().patterns[0].rows[4][0].note == nt::engine::kNoteOff);

    // One undo entry per batch: undoing restores both writes at once.
    const json range = lb.execute(json::array(
        {{{"op", "setRange"},
          {"patternId", 0},
          {"rowStart", 8},
          {"channels", {1, 2}},
          {"cells", {{{{"note", 52}}, {{"note", 55}}}, {{{"note", 52}}, {{"note", 55}}}}}}}));
    REQUIRE(range.at("ok").get<bool>());
    CHECK(lb.session.project().patterns[0].rows[8][1].note == 52);
    CHECK(lb.session.project().patterns[0].rows[9][2].note == 55);
    REQUIRE(lb.session.undo().undo());
    CHECK(lb.session.project().patterns[0].rows[8][1].note == 0);
    CHECK(lb.session.project().patterns[0].rows[9][2].note == 0);
}

TEST_CASE("local api rejects bogus ids with typed errors", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    SECTION("unknown pattern id") {
        const json& err = first_error(lb.execute(json::array({{{"op", "setCell"},
                                                               {"patternId", 99},
                                                               {"row", 0},
                                                               {"channel", 0},
                                                               {"cell", {{"note", 1}}}}})));
        CHECK(err.at("code").get<std::string>() == "notFound");
        CHECK(err.at("index").get<int>() == 0);
    }

    SECTION("row out of range") {
        const json& err = first_error(lb.execute(json::array({{{"op", "setCell"},
                                                               {"patternId", 0},
                                                               {"row", 9999},
                                                               {"channel", 0},
                                                               {"cell", {{"note", 1}}}}})));
        CHECK(err.at("code").get<std::string>() == "outOfBounds");
    }

    SECTION("unknown op") {
        const json& err = first_error(lb.execute(json::array({{{"op", "frobnicate"}}})));
        CHECK(err.at("code").get<std::string>() == "invalidOp");
    }

    SECTION("known-but-unsupported web op") {
        const json& err = first_error(lb.execute(json::array({{{"op", "renamePattern"}}})));
        CHECK(err.at("code").get<std::string>() == "unsupported");
    }

    SECTION("missing undoDescription") {
        const json result = lb.request(
            "execute", json{{"commands", json::array({{{"op", "play"}}})}, {"opts", {}}});
        CHECK(first_error(result).at("code").get<std::string>() == "missingUndoDescription");
    }

    SECTION("bogus workspace id in an instrument slot is refused, not stored") {
        const json& err =
            first_error(lb.execute(json::array({{{"op", "setInstrumentSlot"},
                                                 {"slot", 1},
                                                 {"entry",
                                                  {{"type", "workspace"},
                                                   {"sampleId", 0},
                                                   {"pluginId", ""},
                                                   {"workspaceId", "ghost-node"}}}}})));
        CHECK(err.at("code").get<std::string>() == "notFound");
        CHECK(lb.session.project().instrument_table.empty()); // nothing stored
    }

    SECTION("bogus workspace node and cable ids") {
        CHECK(first_error(lb.execute(json::array(
                              {{{"op", "removeWorkspaceNode"}, {"workspaceId", "nope"}}})))
                  .at("code")
                  .get<std::string>() == "notFound");
        CHECK(
            first_error(lb.execute(json::array({{{"op", "removeCable"}, {"cableId", "cbl-404"}}})))
                .at("code")
                .get<std::string>() == "notFound");
        CHECK(first_error(lb.execute(json::array(
                              {{{"op", "addCable"},
                                {"source", {{"nodeId", "__tracker-bus"}, {"portId", "chXX"}}},
                                {"dest", {{"nodeId", "__master-in"}, {"portId", "main"}}}}})))
                  .at("code")
                  .get<std::string>() == "notFound");
    }

    SECTION("bogus plugin param target") {
        const json& err = first_error(lb.execute(json::array({{{"op", "setPluginParam"},
                                                               {"workspaceId", "ghost"},
                                                               {"key", "gain"},
                                                               {"value", 0.5}}})));
        CHECK(err.at("code").get<std::string>() == "notFound");
    }

    SECTION("query with bogus pattern id") {
        const json result = lb.read(json{{"op", "getPattern"}, {"patternId", 42}});
        CHECK(first_error(result).at("code").get<std::string>() == "notFound");
    }

    SECTION("a batch with one bad command applies nothing") {
        const json result = lb.execute(json::array({{{"op", "setCell"},
                                                     {"patternId", 0},
                                                     {"row", 0},
                                                     {"channel", 0},
                                                     {"cell", {{"note", 40}}}},
                                                    {{"op", "setCell"},
                                                     {"patternId", 77},
                                                     {"row", 0},
                                                     {"channel", 0},
                                                     {"cell", {{"note", 41}}}}}));
        CHECK_FALSE(result.at("ok").get<bool>());
        CHECK(lb.session.project().patterns[0].rows[0][0].note == 0);
    }
}

TEST_CASE("local api sample upload lands through the decode path", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    const std::vector<std::uint8_t> wav = make_wav_bytes();
    const json result = lb.execute(json::array({{{"op", "loadSampleData"},
                                                 {"slot", 2},
                                                 {"name", "sine.wav"},
                                                 {"dataBase64", base64_encode(wav)}}}));
    REQUIRE(result.at("ok").get<bool>());
    REQUIRE(result.contains("loadedSamples"));
    const json& loaded = result.at("loadedSamples").at(0);
    CHECK(loaded.at("slot").get<int>() == 2);
    CHECK(loaded.at("sample").at("frames").get<int>() > 0);
    CHECK(loaded.at("sample").at("sampleRate").get<int>() == 22050);

    // The decoded buffer is resident in the slot (device-rate).
    const auto* buffer = lb.session.sample_buffer(2);
    REQUIRE(buffer != nullptr);
    CHECK(buffer->frames > 0);
    CHECK(buffer->source_rate == 22050);

    // getSamples lists it (audio payload stripped).
    const json samples = lb.read(json{{"op", "getSamples"}});
    REQUIRE(samples.at("ok").get<bool>());
    CHECK(samples.at("data").at(0).at("id").get<int>() == 2);
    CHECK(samples.at("data").at(0).at("name").get<std::string>() == "SINE");

    SECTION("corrupt base64 is a typed error") {
        const json bad = lb.execute(json::array(
            {{{"op", "loadSampleData"}, {"slot", 3}, {"dataBase64", "!!!not-base64!!!"}}}));
        CHECK(first_error(bad).at("code").get<std::string>() == "invalidField");
    }

    SECTION("valid base64 of non-audio bytes is a typed error") {
        const json bad = lb.execute(json::array(
            {{{"op", "loadSampleData"}, {"slot", 3}, {"dataBase64", "aGVsbG8gd29ybGQ="}}}));
        CHECK(first_error(bad).at("code").get<std::string>() == "ioError");
    }
}

TEST_CASE("local api workspace discovery and cable round trip", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    // Discovery: the builtin nodes and their ports are enumerable —
    // web fix-list #5 (no workspace-ID discovery op existed).
    const json workspace = lb.read(json{{"op", "getWorkspace"}});
    REQUIRE(workspace.at("ok").get<bool>());
    const json& nodes = workspace.at("data").at("nodes");
    std::vector<std::string> ids;
    for (const json& node : nodes) {
        ids.push_back(node.at("workspaceId").get<std::string>());
    }
    CHECK(std::find(ids.begin(), ids.end(), "__tracker-bus") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "__master-in") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "__module-player") != ids.end());
    for (const json& node : nodes) {
        if (node.at("workspaceId").get<std::string>() == "__tracker-bus") {
            bool has_ch01 = false;
            for (const json& port : node.at("outputs")) {
                if (port.at("id").get<std::string>() == "ch01") {
                    has_ch01 = true;
                    CHECK(port.at("kind").get<std::string>() == "audio");
                }
            }
            CHECK(has_ch01);
        }
    }

    // Cable add → visible in discovery → remove → gone.
    const json added =
        lb.execute(json::array({{{"op", "addCable"},
                                 {"source", {{"nodeId", "__tracker-bus"}, {"portId", "ch01"}}},
                                 {"dest", {{"nodeId", "__master-in"}, {"portId", "main"}}},
                                 {"mode", "tap"}}}));
    REQUIRE(added.at("ok").get<bool>());
    REQUIRE(added.contains("createdCableIds"));
    const std::string cable_id = added.at("createdCableIds").at(0).get<std::string>();

    json listed = lb.read(json{{"op", "getWorkspace"}});
    REQUIRE(listed.at("data").at("cables").size() == 1);
    CHECK(listed.at("data").at("cables").at(0).at("id").get<std::string>() == cable_id);
    CHECK(listed.at("data").at("cables").at(0).at("source").at("portId").get<std::string>() ==
          "ch01");

    REQUIRE(lb.execute(json::array({{{"op", "removeCable"}, {"cableId", cable_id}}}))
                .at("ok")
                .get<bool>());
    listed = lb.read(json{{"op", "getWorkspace"}});
    CHECK(listed.at("data").at("cables").empty());

    // Node add/remove round trip.
    const json sum = lb.execute(json::array({{{"op", "addWorkspaceNode"}, {"kind", "sum"}}}));
    REQUIRE(sum.at("ok").get<bool>());
    const std::string sum_id = sum.at("createdNodeIds").at(0).get<std::string>();
    CHECK(lb.session.workspace().find_node(sum_id) != nullptr);
    REQUIRE(lb.execute(json::array({{{"op", "removeWorkspaceNode"}, {"workspaceId", sum_id}}}))
                .at("ok")
                .get<bool>());
    CHECK(lb.session.workspace().find_node(sum_id) == nullptr);
}

TEST_CASE("local api sequence layers round trip", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    REQUIRE(
        lb
            .execute(json::array(
                {{{"op", "addSeqNote"},
                  {"patternId", 0},
                  {"channel", 0},
                  {"layerIndex", 0},
                  {"note",
                   {{"pitch", 60}, {"startTick", 0}, {"durationTicks", 6}, {"velocity", 100}}}}}))
            .at("ok")
            .get<bool>());

    json layer =
        lb.read(json{{"op", "getSeqLayer"}, {"patternId", 0}, {"channel", 0}, {"layerIndex", 0}});
    REQUIRE(layer.at("ok").get<bool>());
    REQUIRE(layer.at("data").at("notes").size() == 1);
    CHECK(layer.at("data").at("notes").at(0).at("pitch").get<int>() == 60);

    REQUIRE(
        lb
            .execute(json::array(
                {{{"op", "updateSeqNote"},
                  {"patternId", 0},
                  {"channel", 0},
                  {"layerIndex", 0},
                  {"noteIndex", 0},
                  {"note",
                   {{"pitch", 64}, {"startTick", 6}, {"durationTicks", 3}, {"velocity", 90}}}}}))
            .at("ok")
            .get<bool>());
    layer =
        lb.read(json{{"op", "getSeqLayer"}, {"patternId", 0}, {"channel", 0}, {"layerIndex", 0}});
    CHECK(layer.at("data").at("notes").at(0).at("pitch").get<int>() == 64);

    REQUIRE(lb.execute(json::array({{{"op", "setSeqLayerEnabled"},
                                     {"patternId", 0},
                                     {"channel", 0},
                                     {"layerIndex", 0},
                                     {"enabled", false}}}))
                .at("ok")
                .get<bool>());
    layer =
        lb.read(json{{"op", "getSeqLayer"}, {"patternId", 0}, {"channel", 0}, {"layerIndex", 0}});
    CHECK_FALSE(layer.at("data").at("enabled").get<bool>());

    const json list = lb.read(json{{"op", "getSeqLayerList"}, {"patternId", 0}});
    REQUIRE(list.at("ok").get<bool>());
    // Layers are preallocated: touching channel 0 materialises all 4.
    CHECK(list.at("data").size() == nt::engine::kMaxSeqLayersPerChannel);
}

TEST_CASE("local api project load and export", "[local_api]") {
    namespace fs = std::filesystem;
    Loopback lb;
    lb.authenticate();

    // A deliberately tiny project (8 rows, 2 channels) so the offline
    // export stays fast; loaded through the API itself.
    nt::engine::TrackerProject project = nt::engine::create_project(2);
    project.patterns[0] = nt::engine::create_pattern(0, 8, 2);
    project.order_list = {0};
    const fs::path ftrk_path = fs::temp_directory_path() / "nt_local_api_test.ftrk";
    const fs::path wav_path = fs::temp_directory_path() / "nt_local_api_test.wav";
    std::string write_error;
    REQUIRE(nt::io::write_ftrk_file(ftrk_path, project, {}, write_error));

    REQUIRE(lb.execute(json::array({{{"op", "loadProject"}, {"path", ftrk_path.string()}}}))
                .at("ok")
                .get<bool>());
    const json summary = lb.read(json{{"op", "getProjectSummary"}});
    CHECK(summary.at("data").at("channels").get<int>() == 2);
    CHECK(summary.at("data").at("patternCount").get<int>() == 1);

    const json exported = lb.execute(json::array({{{"op", "exportProject"},
                                                   {"path", wav_path.string()},
                                                   {"format", "wav"},
                                                   {"sampleRate", 8000},
                                                   {"tailSeconds", 0.1}}}));
    REQUIRE(exported.at("ok").get<bool>());
    REQUIRE(exported.contains("exportResult"));
    CHECK(exported.at("exportResult").at("frames").get<int>() > 0);
    REQUIRE(fs::exists(wav_path));
    CHECK(fs::file_size(wav_path) > 44); // more than a bare WAV header

    // A bad export path is a typed I/O error.
    const json bad = lb.execute(json::array(
        {{{"op", "exportProject"}, {"path", "/nonexistent/dir/out.wav"}, {"format", "wav"}}}));
    CHECK(first_error(bad).at("code").get<std::string>() == "ioError");

    fs::remove(ftrk_path);
    fs::remove(wav_path);
}

TEST_CASE("local api pattern and order-list structure ops", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    const auto pattern_count = [&]() {
        return lb.read(json{{"op", "getProjectSummary"}}).at("data").at("patternCount").get<int>();
    };
    const auto order_list = [&]() {
        return lb.read(json{{"op", "getOrderList"}}).at("data").get<std::vector<int>>();
    };

    REQUIRE(pattern_count() == 1);
    REQUIRE(order_list() == std::vector<int>{0});

    SECTION("createPattern returns the new id and honours a custom row count") {
        const json r = lb.execute(json::array({{{"op", "createPattern"}, {"rows", 32}}}));
        REQUIRE(r.at("ok").get<bool>());
        CHECK(r.at("createdPatternIds") == json::array({1}));
        CHECK(pattern_count() == 2);
        const json pat = lb.read(json{{"op", "getPattern"}, {"patternId", 1}});
        CHECK(pat.at("data").at("rows").size() == 32);
    }

    SECTION("insert / setOrderList / remove round-trip") {
        REQUIRE(lb.execute(json::array({{{"op", "createPattern"}}})).at("ok").get<bool>()); // id 1
        REQUIRE(lb.execute(json::array({{{"op", "insertOrderAt"}, {"index", 1}, {"patternId", 1}}}))
                    .at("ok")
                    .get<bool>());
        CHECK(order_list() == std::vector<int>{0, 1});
        REQUIRE(lb.execute(json::array({{{"op", "setOrderList"}, {"orderList", {1, 0, 1}}}}))
                    .at("ok")
                    .get<bool>());
        CHECK(order_list() == std::vector<int>{1, 0, 1});
        REQUIRE(lb.execute(json::array({{{"op", "removeOrderAt"}, {"index", 0}}}))
                    .at("ok")
                    .get<bool>());
        CHECK(order_list() == std::vector<int>{0, 1});
    }

    SECTION("deletePattern remaps the order list") {
        lb.execute(json::array({{{"op", "createPattern"}}})); // id 1
        lb.execute(json::array({{{"op", "createPattern"}}})); // id 2
        lb.execute(json::array({{{"op", "setOrderList"}, {"orderList", {0, 1, 2, 1}}}}));
        REQUIRE(lb.execute(json::array({{{"op", "deletePattern"}, {"patternId", 1}}}))
                    .at("ok")
                    .get<bool>());
        CHECK(pattern_count() == 2);
        CHECK(order_list() == std::vector<int>{0, 1});
    }

    SECTION("refusals return typed errors") {
        CHECK(first_error(lb.execute(json::array({{{"op", "deletePattern"}, {"patternId", 0}}})))
                  .at("code")
                  .get<std::string>() == "invalidOp"); // last pattern
        CHECK(first_error(lb.execute(json::array({{{"op", "removeOrderAt"}, {"index", 0}}})))
                  .at("code")
                  .get<std::string>() == "invalidOp"); // last order entry
        CHECK(first_error(lb.execute(json::array(
                              {{{"op", "insertOrderAt"}, {"index", 0}, {"patternId", 99}}})))
                  .at("code")
                  .get<std::string>() == "notFound");
        CHECK(
            first_error(lb.execute(json::array({{{"op", "setOrderList"}, {"orderList", {0, 5}}}})))
                .at("code")
                .get<std::string>() == "outOfBounds");
        CHECK(first_error(lb.execute(json::array(
                              {{{"op", "resizePattern"}, {"patternId", 0}, {"rows", 999}}})))
                  .at("code")
                  .get<std::string>() == "invalidField");
    }
}

TEST_CASE("local api schema discovery", "[local_api]") {
    Loopback lb;
    lb.authenticate();

    const json schema = lb.read(json{{"op", "getSchema"}});
    REQUIRE(schema.at("ok").get<bool>());
    CHECK(schema.at("data").at("version").get<std::string>() == nt::api::kLocalApiVersion);
    CHECK(!schema.at("data").at("commands").empty());
    CHECK(!schema.at("data").at("queries").empty());
}
