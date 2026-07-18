// Settings round-trip for the Stage 28 asset-browser fields (library
// roots, recent-projects MRU with metadata, favourites) and the pure
// list-management / classification helpers behind the PROJECTS and
// LIBRARY windows. The UI itself is screenshot-verified; this pins the
// persistence contract and the logic the windows lean on.
#include "app/project_session.h"
#include "audio/audio_engine.h"
#include "io/settings.h"
#include "ui/library_view.h"
#include "ui/projects_view.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_settings_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

nt::io::RecentProject make_recent(const std::string& path, const std::string& name, int channels,
                                  int patterns, std::int64_t when, bool pinned = false) {
    nt::io::RecentProject r;
    r.path = path;
    r.name = name;
    r.channels = channels;
    r.patterns = patterns;
    r.last_opened = when;
    r.pinned = pinned;
    return r;
}

} // namespace

TEST_CASE("settings round-trips the asset-browser fields", "[settings][asset]") {
    const auto path = temp_settings_path("nt_settings_asset_test.json");

    nt::io::Settings out;
    out.library_roots = {"/music/samples", "/music/plugins"};
    out.favourite_paths = {"/music/samples/kick.wav", "/music/plugins/lead.ntins"};
    out.recent_projects = {
        make_recent("/songs/a.ftrk", "SONG A", 4, 8, 1000, /*pinned=*/true),
        make_recent("/songs/b.ftrk", "SONG B", 8, 16, 2000),
    };

    REQUIRE(nt::io::save_settings(out, path));
    const nt::io::Settings in = nt::io::load_settings(path);

    CHECK(in.library_roots == out.library_roots);
    CHECK(in.favourite_paths == out.favourite_paths);
    REQUIRE(in.recent_projects.size() == 2);
    CHECK(in.recent_projects == out.recent_projects);
    CHECK(in.recent_projects[0].pinned);
    CHECK(in.recent_projects[0].channels == 4);
    CHECK(in.recent_projects[1].patterns == 16);

    std::filesystem::remove(path);
}

TEST_CASE("settings load is tolerant of the new fields' absence", "[settings][asset]") {
    const auto path = temp_settings_path("nt_settings_tolerant_test.json");
    // A minimal legacy file with none of the Stage 28 keys.
    {
        std::ofstream file(path);
        file << R"({"schema":1,"theme_id":"arctic-light"})" << '\n';
    }
    const nt::io::Settings in = nt::io::load_settings(path);
    CHECK(in.library_roots.empty());
    CHECK(in.favourite_paths.empty());
    CHECK(in.recent_projects.empty());
    std::filesystem::remove(path);
}

TEST_CASE("settings load drops malformed recent entries, keeps valid ones", "[settings][asset]") {
    const auto path = temp_settings_path("nt_settings_malformed_test.json");
    {
        std::ofstream file(path);
        // One good entry, one non-object, one object without a path.
        file << R"({"recent_projects":[)"
             << R"({"path":"/songs/good.ftrk","name":"GOOD","channels":4,"patterns":2},)"
             << R"(42,)" << R"({"name":"NO PATH","channels":8}]})" << '\n';
    }
    const nt::io::Settings in = nt::io::load_settings(path);
    REQUIRE(in.recent_projects.size() == 1);
    CHECK(in.recent_projects[0].path == "/songs/good.ftrk");
    CHECK(in.recent_projects[0].name == "GOOD");
    std::filesystem::remove(path);
}

TEST_CASE("record_recent_project dedups, refreshes and keeps pins", "[projects][mru]") {
    using nt::ui::record_recent_project;
    std::vector<nt::io::RecentProject> recents;

    record_recent_project(recents, make_recent("/a.ftrk", "A", 4, 1, 100), 20);
    record_recent_project(recents, make_recent("/b.ftrk", "B", 4, 1, 200), 20);
    REQUIRE(recents.size() == 2);
    CHECK(recents.front().path == "/b.ftrk"); // most-recent first

    SECTION("same path refreshes in place (no duplicate) and re-caches metadata") {
        record_recent_project(recents, make_recent("/a.ftrk", "A2", 8, 9, 300), 20);
        REQUIRE(recents.size() == 2);
        const auto& a = recents.front();
        CHECK(a.path == "/a.ftrk");
        CHECK(a.name == "A2");
        CHECK(a.channels == 8);
        CHECK(a.patterns == 9);
    }

    SECTION("a pin survives a refresh and sorts ahead of newer unpinned entries") {
        REQUIRE(nt::ui::toggle_recent_pin(recents, "/a.ftrk"));
        CHECK(recents.front().path == "/a.ftrk"); // pinned floats up
        // Refresh /a with fresh metadata: still pinned, still first.
        record_recent_project(recents, make_recent("/a.ftrk", "A3", 2, 2, 50), 20);
        CHECK(recents.front().path == "/a.ftrk");
        CHECK(recents.front().pinned);
    }
}

TEST_CASE("record_recent_project caps the unpinned tail, never pinned", "[projects][mru]") {
    using nt::ui::record_recent_project;
    std::vector<nt::io::RecentProject> recents;
    constexpr std::size_t kCap = 3;

    // Pin one old project up front.
    record_recent_project(recents, make_recent("/pinned.ftrk", "P", 4, 1, 1), kCap);
    REQUIRE(nt::ui::toggle_recent_pin(recents, "/pinned.ftrk"));

    // Push six unpinned; only kCap of them may remain.
    for (int i = 0; i < 6; ++i) {
        const std::string p = "/u" + std::to_string(i) + ".ftrk";
        record_recent_project(recents, make_recent(p, "U", 4, 1, 100 + i), kCap);
    }

    std::size_t pinned = 0;
    std::size_t unpinned = 0;
    for (const auto& r : recents) {
        (r.pinned ? pinned : unpinned) += 1;
    }
    CHECK(pinned == 1);
    CHECK(unpinned == kCap);
    // The pinned entry is retained despite being the oldest by timestamp.
    CHECK(recents.front().path == "/pinned.ftrk");
    // The oldest unpinned (/u0, /u1, /u2) were evicted; newest survive.
    const bool has_newest = std::any_of(recents.begin(), recents.end(),
                                        [](const auto& r) { return r.path == "/u5.ftrk"; });
    const bool has_oldest = std::any_of(recents.begin(), recents.end(),
                                        [](const auto& r) { return r.path == "/u0.ftrk"; });
    CHECK(has_newest);
    CHECK_FALSE(has_oldest);
}

TEST_CASE("a missing-path entry is retained (never auto-deleted)", "[projects][mru]") {
    std::vector<nt::io::RecentProject> recents;
    nt::ui::record_recent_project(recents, make_recent("/definitely/not/here.ftrk", "X", 4, 1, 1),
                                  20);
    // The list-management layer keeps it; the window greys it and offers
    // a manual remove — a drive may just be unmounted.
    REQUIRE(recents.size() == 1);
    CHECK_FALSE(std::filesystem::exists(recents.front().path));
}

TEST_CASE("classify_asset maps extensions case-insensitively", "[library][classify]") {
    using nt::ui::AssetKind;
    using nt::ui::classify_asset;
    CHECK(classify_asset("/x/kick.wav") == AssetKind::kSample);
    CHECK(classify_asset("/x/pad.OGG") == AssetKind::kSample);
    CHECK(classify_asset("/x/loop.Mp3") == AssetKind::kSample);
    CHECK(classify_asset("/x/bass.ntins") == AssetKind::kNtpArchive);
    CHECK(classify_asset("/x/fx.NTSFX") == AssetKind::kNtpArchive);
    CHECK(classify_asset("/x/song.ftrk") == AssetKind::kOther);
    CHECK(classify_asset("/x/readme") == AssetKind::kOther);
}

TEST_CASE("toggle_favourite adds then removes", "[library][favourites]") {
    std::vector<std::string> favs;
    CHECK(nt::ui::toggle_favourite(favs, "/x/kick.wav"));
    REQUIRE(favs.size() == 1);
    CHECK_FALSE(nt::ui::toggle_favourite(favs, "/x/kick.wav"));
    CHECK(favs.empty());
}

TEST_CASE("new-from-template channel count backs the PROJECTS buttons", "[projects][template]") {
    // The 4ch / 8ch template buttons call ProjectSession::new_project(n);
    // this pins the channel-count setter the 8ch template needs (the
    // engine is never started — device-independent).
    nt::audio::AudioEngine audio;
    nt::app::ProjectSession session(audio);

    session.new_project(8);
    CHECK(session.project().channels == 8);
    REQUIRE_FALSE(session.project().patterns.empty());
    CHECK(session.project().patterns[0].rows[0].size() == 8);

    session.new_project(4);
    CHECK(session.project().channels == 4);

    session.new_project();
    CHECK(session.project().channels == 4); // default overload

    session.new_project(999);
    CHECK(session.project().channels == nt::engine::kMaxChannels); // clamped
}
