// Stage 19 POVR verification: the wire block round-trips structured
// overrides (dedup re-expanded) and carries unknown block versions
// verbatim; a full session save→load cycle reproduces the same
// override audio (hash-keyed buffer equality) and preserves entries
// that never resolve to an instance.
#include "app/project_session.h"
#include "io/ftrk_reader.h"
#include "io/ftrk_writer.h"
#include "io/sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <miniz.h>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 48000;

// Minimal 16-bit mono PCM WAV (same generator as ntp_test).
std::vector<std::uint8_t> make_wav(float freq, float seconds, std::uint32_t rate) {
    const auto frames = static_cast<std::uint32_t>(seconds * static_cast<float>(rate));
    std::vector<std::uint8_t> bytes(44 + (static_cast<std::size_t>(frames) * 2));
    auto put32 = [&bytes](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            bytes[at + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        }
    };
    auto put16 = [&bytes](std::size_t at, std::uint16_t v) {
        bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
        bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36 + (frames * 2));
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, rate);
    put32(28, rate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float s =
            std::sin(2.0F * 3.14159265F * freq * static_cast<float>(i) / static_cast<float>(rate));
        const auto v = static_cast<std::int16_t>(s * 20000.0F);
        put16(44 + (static_cast<std::size_t>(i) * 2), static_cast<std::uint16_t>(v));
    }
    return bytes;
}

std::vector<std::uint8_t>
make_zip(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files) {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0) == MZ_TRUE);
    for (const auto& [name, data] : files) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                                      MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
    }
    void* buf = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size) == MZ_TRUE);
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(buf),
                                  static_cast<std::uint8_t*>(buf) + size);
    mz_zip_writer_end(&zip);
    mz_free(buf);
    return out;
}

std::filesystem::path write_temp(const char* name, const std::vector<std::uint8_t>& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/file seam
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return path;
}

// A user-slot instrument: one user-assignable zone with a 500 Hz
// fallback, mapped to note 60.
std::vector<std::uint8_t> make_slot_plugin() {
    const std::string manifest = R"({
      "ntp": 1, "id": "test.povr.kit", "name": "POVR KIT", "type": "instrument",
      "requires": ["userSamples"],
      "graph": {
        "nodes": [{"id": "s", "type": "sampler", "zones": [
          {"userAssignable": true, "slotId": "break", "slotLabel": "BREAK",
           "fallbackFile": "fallback.wav", "rootKey": 60,
           "keyRange": {"lo": 60, "hi": 60}}
        ]}],
        "connections": [{"from": "s", "to": "output"}]
      }
    })";
    return make_zip({
        {"plugin.json", {manifest.begin(), manifest.end()}},
        {"fallback.wav", make_wav(500.0F, 0.1F, kRate)},
    });
}

} // namespace

TEST_CASE("sha256 matches the web's WebCrypto digests", "[povr]") {
    // FIPS 180-4 vectors — the hashes must agree byte-for-byte with
    // the web app's crypto.subtle output or shared files cannot dedup.
    const std::string abc = "abc";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/text seam
    CHECK(nt::io::sha256_hex_prefixed(reinterpret_cast<const std::uint8_t*>(abc.data()),
                                      abc.size()) ==
          "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(nt::io::sha256_hex_prefixed(nullptr, 0) ==
          "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Two blocks + length spill into a third: the multi-block path.
    const std::string long_input(120, 'a');
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — byte/text seam
    CHECK(nt::io::sha256_hex_prefixed(reinterpret_cast<const std::uint8_t*>(long_input.data()),
                                      long_input.size()) ==
          "sha256:2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

TEST_CASE("POVR block round-trips structured overrides and raw unknowns", "[ftrk][povr]") {
    const nt::engine::TrackerProject project = nt::engine::create_project(4);

    SECTION("block v1: dedup'd bytes re-expand per entry") {
        const std::vector<std::uint8_t> wav = {9, 8, 7, 6, 5};
        const std::string hash = nt::io::sha256_hex_prefixed(wav.data(), wav.size());
        nt::io::FtrkWriteExtras extras;
        // Two instances, three slots; the same sample sits in two
        // slots — its bytes must appear in the block exactly once.
        extras.povr.push_back({.instance_id = "plg-1",
                               .slot_id = "kick",
                               .hash = hash,
                               .name = "kick.wav",
                               .sample_rate = 44100,
                               .channels = 1,
                               .duration = 0.25F,
                               .bytes = wav});
        extras.povr.push_back({.instance_id = "plg-1",
                               .slot_id = "snare",
                               .hash = "sha256:other",
                               .name = "snare.wav",
                               .sample_rate = 48000,
                               .channels = 2,
                               .duration = 0.5F,
                               .bytes = {1, 2, 3}});
        extras.povr.push_back({.instance_id = "plg-2",
                               .slot_id = "kick",
                               .hash = hash,
                               .name = "kick.wav",
                               .sample_rate = 44100,
                               .channels = 1,
                               .duration = 0.25F,
                               .bytes = wav});
        extras.pprs_json = R"([{"instanceId":"plg-1","projectPresets":[]}])";

        const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, extras);
        std::string error;
        auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
        INFO(error);
        REQUIRE(result.has_value());
        REQUIRE(result->extras.povr_overrides.size() == 3);
        CHECK(result->extras.povr_raw.empty());
        const auto& loaded = result->extras.povr_overrides;
        CHECK(loaded[0].instance_id == "plg-1");
        CHECK(loaded[0].slot_id == "kick");
        CHECK(loaded[0].hash == hash);
        CHECK(loaded[0].name == "kick.wav");
        CHECK(loaded[0].sample_rate == 44100);
        CHECK(loaded[0].channels == 1);
        CHECK(loaded[0].duration == 0.25F);
        CHECK(loaded[0].bytes == wav);
        CHECK(loaded[1].hash == "sha256:other");
        CHECK(loaded[1].bytes == std::vector<std::uint8_t>({1, 2, 3}));
        CHECK(loaded[2].instance_id == "plg-2");
        CHECK(loaded[2].bytes == wav); // re-expanded from the dedup ref
        // The wire carries the shared payload once.
        int occurrences = 0;
        for (std::size_t i = 0; i + wav.size() <= bytes.size(); ++i) {
            if (std::memcmp(bytes.data() + i, wav.data(), wav.size()) == 0) {
                ++occurrences;
            }
        }
        CHECK(occurrences == 1);
        // PPRS after the block still parses (POVR's extent is exact).
        CHECK(result->extras.pprs_json == extras.pprs_json);
    }

    SECTION("unknown block version carries verbatim, byte-exact") {
        // A future POVR (version 2) this reader cannot interpret.
        std::vector<std::uint8_t> raw = {'P', 'O', 'V', 'R', 2, 0xDE, 0xAD, 0xBE, 0xEF, 42};
        nt::io::FtrkWriteExtras extras;
        extras.povr_raw = raw;
        extras.pprs_json = "[]"; // a following block bounds the carry

        const std::vector<std::uint8_t> bytes = nt::io::write_ftrk(project, extras);
        std::string error;
        auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
        REQUIRE(result.has_value());
        CHECK(result->extras.povr_overrides.empty());
        CHECK(result->extras.povr_raw == raw);
        CHECK(result->extras.pprs_json == "[]");

        // Second generation: still byte-exact.
        nt::io::FtrkWriteExtras again;
        again.povr_raw = result->extras.povr_raw;
        const std::vector<std::uint8_t> bytes2 = nt::io::write_ftrk(project, again);
        auto result2 = nt::io::read_ftrk(bytes2.data(), bytes2.size(), error);
        REQUIRE(result2.has_value());
        CHECK(result2->extras.povr_raw == raw);
    }
}

TEST_CASE("session round-trips live slot overrides through POVR", "[session][povr]") {
    nt::audio::AudioEngine audio; // not started — device-independent
    const auto plugin_path = write_temp("nt_povr_kit.ntins", make_slot_plugin());
    const auto user_wav_bytes = make_wav(900.0F, 0.1F, kRate);
    const auto user_wav_path = write_temp("nt_povr_user.wav", user_wav_bytes);
    const auto project_path = std::filesystem::temp_directory_path() / "nt_povr_roundtrip.ftrk";

    std::string workspace_id;
    std::vector<float> saved_audio;
    {
        nt::app::ProjectSession session(audio);
        REQUIRE_FALSE(session.load_plugin_file(plugin_path).empty());
        workspace_id = session.add_plugin_node("test.povr.kit");
        REQUIRE_FALSE(workspace_id.empty());

        // Slot/typo guards fail cleanly before any structural work.
        CHECK_FALSE(session.set_plugin_sample_override(workspace_id, "ghost", user_wav_path));
        CHECK_FALSE(session.set_plugin_sample_override("plg-ghost", "break", user_wav_path));

        REQUIRE(session.set_plugin_sample_override(workspace_id, "break", user_wav_path));
        nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
        REQUIRE(instance != nullptr);
        const auto it = instance->slot_overrides().find("break");
        REQUIRE(it != instance->slot_overrides().end());
        CHECK(it->second.hash ==
              nt::io::sha256_hex_prefixed(user_wav_bytes.data(), user_wav_bytes.size()));
        CHECK(it->second.name == "nt_povr_user.wav");
        CHECK(it->second.bytes == user_wav_bytes);
        REQUIRE(it->second.buffer != nullptr);
        saved_audio = it->second.buffer->interleaved;

        REQUIRE(session.save_ftrk(project_path));
    }

    {
        nt::app::ProjectSession session(audio);
        REQUIRE(session.load_ftrk(project_path));
        nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
        REQUIRE(instance != nullptr);
        const auto it = instance->slot_overrides().find("break");
        REQUIRE(it != instance->slot_overrides().end());
        CHECK(it->second.hash ==
              nt::io::sha256_hex_prefixed(user_wav_bytes.data(), user_wav_bytes.size()));
        CHECK(it->second.bytes == user_wav_bytes);
        // Same bytes decoded through the same path at the same rate:
        // the resident audio is identical, not merely similar.
        REQUIRE(it->second.buffer != nullptr);
        CHECK(it->second.buffer->interleaved == saved_audio);

        // Clearing reverts to fallback and survives a save without the
        // override.
        REQUIRE(session.clear_plugin_sample_override(workspace_id, "break"));
        REQUIRE(session.save_ftrk(project_path));
    }

    {
        nt::app::ProjectSession session(audio);
        REQUIRE(session.load_ftrk(project_path));
        nt::plugins::NtpInstance* instance = session.plugin_instance(workspace_id);
        REQUIRE(instance != nullptr);
        CHECK(instance->slot_overrides().empty());
    }

    std::filesystem::remove(plugin_path);
    std::filesystem::remove(user_wav_path);
    std::filesystem::remove(project_path);
}

TEST_CASE("unresolved POVR entries survive a session load-save cycle", "[session][povr]") {
    // A file whose POVR references an instance this session will never
    // have (the plugin is not bundled) — the entry must pass through
    // the save untouched rather than vanish.
    nt::io::FtrkWriteExtras extras;
    extras.povr.push_back({.instance_id = "plg-ghost",
                           .slot_id = "break",
                           .hash = "sha256:feed",
                           .name = "lost.wav",
                           .sample_rate = 22050,
                           .channels = 1,
                           .duration = 1.5F,
                           .bytes = {4, 4, 4, 4}});
    const nt::engine::TrackerProject project = nt::engine::create_project(4);
    const auto path = write_temp("nt_povr_ghost.ftrk", nt::io::write_ftrk(project, extras));

    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);
    REQUIRE(session.load_ftrk(path));
    const auto out_path = std::filesystem::temp_directory_path() / "nt_povr_ghost_out.ftrk";
    REQUIRE(session.save_ftrk(out_path));

    std::string error;
    std::ifstream file(out_path, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    auto result = nt::io::read_ftrk(bytes.data(), bytes.size(), error);
    REQUIRE(result.has_value());
    REQUIRE(result->extras.povr_overrides.size() == 1);
    const auto& entry = result->extras.povr_overrides[0];
    CHECK(entry.instance_id == "plg-ghost");
    CHECK(entry.slot_id == "break");
    CHECK(entry.hash == "sha256:feed");
    CHECK(entry.name == "lost.wav");
    CHECK(entry.sample_rate == 22050);
    CHECK(entry.channels == 1);
    CHECK(entry.duration == 1.5F);
    CHECK(entry.bytes == std::vector<std::uint8_t>({4, 4, 4, 4}));

    std::filesystem::remove(path);
    std::filesystem::remove(out_path);
}
