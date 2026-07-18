// ui/sample_browser_view — the SAMPLE BROWSER window: filesystem
// navigation for loading samples into tracker slots, replacing the old
// type-a-path load field (and widening the web app's Load Sample
// window, which could only see the project's samples/ directory).
// Single click selects a file and auditions it; double click loads it
// into the slot selected in the SAMPLES window; directories open on
// double click. The listing shows directories first, then files with
// the extensions audio/load_sample_memory actually decodes.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::ui {

class SampleBrowserView {
public:
    SampleBrowserView();

    // Audition routes through ProjectSession::preview_file — a
    // reclaimer-backed decode-and-play, so the browser holds no sample
    // buffers of its own (the process-lifetime audition pool is gone).
    void draw(app::ProjectSession& session, int target_slot, const Theme& theme);

private:
    struct Entry {
        std::filesystem::path path;
        std::string name; // display filename
        std::uintmax_t bytes = 0;
        bool is_dir = false;
    };

    void navigate_to(const std::filesystem::path& dir);
    void refresh();
    void audition(app::ProjectSession& session, const Entry& entry);

    std::filesystem::path dir_; // remembered across frames (in-memory only)
    std::vector<Entry> entries_;
    std::string list_error_;
    std::string status_;
    std::filesystem::path selected_;
    bool dirty_ = true; // listing needs a rescan
};

} // namespace nt::ui
