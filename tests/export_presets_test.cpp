// Export preset store: full ExportOptions round-trip through the JSON
// file (save → fresh store → equal options), rename/delete persistence,
// and the built-in read-only guarantees.
#include "io/export_presets.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path temp_store_path() {
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() /
           ("nt_export_presets_test_" + std::to_string(stamp) + ".json");
}

// Every field off its default, so a dropped key fails the comparison.
nt::io::ExportOptions make_full_options() {
    nt::io::ExportOptions options;
    options.format = nt::io::ExportFormat::kOgg;
    options.wav_depth = nt::io::WavDepth::kFloat32;
    options.ogg_quality = 0.73F;
    options.mp3_bitrate_kbps = 256;
    options.mp3_quality = 5;
    options.sample_rate = 44100;
    options.tail_seconds = 4.5;
    options.start_order = 2;
    options.end_order = 7;
    options.stem_mask = 0b1010;
    options.stem_zip = true;
    options.post.fade_in_seconds = 1.5;
    options.post.fade_out_seconds = 2.25;
    options.post.fade_in_shape = nt::io::FadeShape::kEqualPower;
    options.post.fade_out_shape = nt::io::FadeShape::kEqualPower;
    options.post.normalize = nt::io::NormalizeMode::kTruePeak;
    options.post.normalize_target_db = -9.5;
    options.metadata.title = "Round Trip";
    options.metadata.artist = "nt tests";
    options.metadata.album = "Fixtures";
    options.metadata.date = "2026";
    options.metadata.comment = "save/reload equality";
    return options;
}

void require_equal(const nt::io::ExportOptions& a, const nt::io::ExportOptions& b) {
    REQUIRE(a.format == b.format);
    REQUIRE(a.wav_depth == b.wav_depth);
    REQUIRE(a.ogg_quality == b.ogg_quality);
    REQUIRE(a.mp3_bitrate_kbps == b.mp3_bitrate_kbps);
    REQUIRE(a.mp3_quality == b.mp3_quality);
    REQUIRE(a.sample_rate == b.sample_rate);
    REQUIRE(a.tail_seconds == b.tail_seconds);
    REQUIRE(a.start_order == b.start_order);
    REQUIRE(a.end_order == b.end_order);
    REQUIRE(a.stem_mask == b.stem_mask);
    REQUIRE(a.stem_zip == b.stem_zip);
    REQUIRE(a.post.fade_in_seconds == b.post.fade_in_seconds);
    REQUIRE(a.post.fade_out_seconds == b.post.fade_out_seconds);
    REQUIRE(a.post.fade_in_shape == b.post.fade_in_shape);
    REQUIRE(a.post.fade_out_shape == b.post.fade_out_shape);
    REQUIRE(a.post.normalize == b.post.normalize);
    REQUIRE(a.post.normalize_target_db == b.post.normalize_target_db);
    REQUIRE(a.metadata.title == b.metadata.title);
    REQUIRE(a.metadata.artist == b.metadata.artist);
    REQUIRE(a.metadata.album == b.metadata.album);
    REQUIRE(a.metadata.date == b.metadata.date);
    REQUIRE(a.metadata.comment == b.metadata.comment);
}

} // namespace

TEST_CASE("export preset store round-trips the full options bundle", "[export_presets]") {
    const std::filesystem::path file = temp_store_path();
    const nt::io::ExportOptions options = make_full_options();

    nt::io::ExportPresetStore store(file);
    REQUIRE(store.user_presets().empty());
    const std::string id = store.save_user_preset("ROUNDTRIP", options);
    REQUIRE_FALSE(id.empty());

    // A fresh store sees only the file — save → reload → equal options.
    nt::io::ExportPresetStore reloaded(file);
    REQUIRE(reloaded.user_presets().size() == 1);
    const nt::io::ExportPreset* preset = reloaded.find(id);
    REQUIRE(preset != nullptr);
    REQUIRE(preset->name == "ROUNDTRIP");
    require_equal(preset->options, options);

    // Rename persists.
    REQUIRE(reloaded.rename_user_preset(id, "RENAMED"));
    nt::io::ExportPresetStore renamed(file);
    REQUIRE(renamed.find(id) != nullptr);
    REQUIRE(renamed.find(id)->name == "RENAMED");

    // Delete persists.
    REQUIRE(renamed.delete_user_preset(id));
    REQUIRE(renamed.find(id) == nullptr);
    nt::io::ExportPresetStore emptied(file);
    REQUIRE(emptied.user_presets().empty());

    std::filesystem::remove(file);
}

TEST_CASE("built-in export presets are resolvable and read-only", "[export_presets]") {
    const std::filesystem::path file = temp_store_path();
    nt::io::ExportPresetStore store(file);

    REQUIRE_FALSE(nt::io::builtin_export_presets().empty());
    const nt::io::ExportPreset* wav16 = store.find("default-wav16");
    REQUIRE(wav16 != nullptr);
    REQUIRE(wav16->options.format == nt::io::ExportFormat::kWav);
    REQUIRE(wav16->options.wav_depth == nt::io::WavDepth::kPcm16);
    REQUIRE(nt::io::is_builtin_export_preset("stems-zip"));

    REQUIRE_FALSE(store.delete_user_preset("default-wav16"));
    REQUIRE_FALSE(store.rename_user_preset("default-wav16", "hijack"));
    REQUIRE_FALSE(store.delete_user_preset("no-such-id"));

    // A file entry reusing a built-in id can never shadow the built-in.
    {
        std::ofstream out(file);
        out << R"([{"id":"default-wav16","name":"shadow","options":{"format":"mp3"}}])" << '\n';
    }
    store.reload();
    REQUIRE(store.user_presets().empty());
    REQUIRE(store.find("default-wav16")->options.format == nt::io::ExportFormat::kWav);

    std::filesystem::remove(file);
}

TEST_CASE("export preset store tolerates a malformed file", "[export_presets]") {
    const std::filesystem::path file = temp_store_path();
    {
        std::ofstream out(file);
        out << "{ not json";
    }
    const nt::io::ExportPresetStore store(file);
    REQUIRE(store.user_presets().empty());
    std::filesystem::remove(file);
}
