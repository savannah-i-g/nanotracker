// ui/projects_view — the PROJECTS window: a persisted recent-projects
// list (MRU) plus new-from-template shortcuts. Entries cache the
// project's name and channel/pattern counts at open/save time so the
// list renders without touching disk; opening one routes through the
// same ProjectSession::load_file path the FILE menu uses, and re-caches
// the metadata on success. Pinned entries sort first and never evict;
// the unpinned tail is capped and evicts oldest. Missing files are shown
// greyed with a manual remove (a drive may just be unmounted — the list
// never deletes an entry on the user's behalf).
//
// The list lives in io::Settings; the pure list-management helpers below
// are unit-tested, and the window is screenshot-verified.
#pragma once

#include "app/project_session.h"
#include "io/settings.h"
#include "ui/theme.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace nt::ui {

// Unpinned entries beyond this many are evicted (oldest first); pinned
// entries are never counted against the cap.
inline constexpr std::size_t kUnpinnedRecentCap = 20;

// The list-management helpers are inline (header-defined) so the test
// binary can exercise them without linking the UI/imgui layer.

// Orders the list in place: pinned first, then most-recent first. Stable
// within each group so equal timestamps keep insertion order.
inline void sort_recent_projects(std::vector<io::RecentProject>& recents) {
    std::stable_sort(recents.begin(), recents.end(),
                     [](const io::RecentProject& a, const io::RecentProject& b) {
                         if (a.pinned != b.pinned) {
                             return a.pinned; // pinned entries first
                         }
                         return a.last_opened > b.last_opened; // most recent first
                     });
}

// Inserts or refreshes `entry` (matched by path) at the recency front.
// An existing entry keeps its pinned flag and is updated in place;
// otherwise the entry is added. Afterwards the list is sorted and
// unpinned entries beyond `unpinned_cap` (oldest first) are dropped.
inline void record_recent_project(std::vector<io::RecentProject>& recents, io::RecentProject entry,
                                  std::size_t unpinned_cap) {
    const auto existing =
        std::find_if(recents.begin(), recents.end(),
                     [&](const io::RecentProject& r) { return r.path == entry.path; });
    if (existing != recents.end()) {
        entry.pinned = existing->pinned; // a refresh never silently unpins
        *existing = std::move(entry);
    } else {
        recents.push_back(std::move(entry));
    }
    sort_recent_projects(recents);

    // Evict from the tail (oldest unpinned after the sort); pinned
    // entries survive regardless of how many accumulate.
    std::size_t unpinned = 0;
    for (const io::RecentProject& r : recents) {
        unpinned += r.pinned ? 0 : 1;
    }
    while (unpinned > unpinned_cap && !recents.empty() && !recents.back().pinned) {
        recents.pop_back();
        --unpinned;
    }
}

// Flips the pinned flag of the entry at `path` and re-sorts. Returns the
// resulting pinned state (false when the path is absent).
inline bool toggle_recent_pin(std::vector<io::RecentProject>& recents, const std::string& path) {
    const auto entry = std::find_if(recents.begin(), recents.end(),
                                    [&](const io::RecentProject& r) { return r.path == path; });
    if (entry == recents.end()) {
        return false;
    }
    entry->pinned = !entry->pinned;
    const bool pinned = entry->pinned;
    sort_recent_projects(recents);
    return pinned;
}

class ProjectsView {
public:
    // `open` is the VIEW-menu visibility flag; the window's close box
    // clears it (the DEBUG/HISTORY window contract).
    void draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme, bool& open);

    // Records a just-opened or just-saved project into the MRU. Every
    // open/save surface (FILE menu, this window, the CLI) funnels through
    // here so recording lives in one place; metadata is snapshotted from
    // the session's live project. `path` is resolved to absolute.
    static void record(io::Settings& settings, const app::ProjectSession& session,
                       const std::filesystem::path& path);

private:
    std::string status_;
};

} // namespace nt::ui
