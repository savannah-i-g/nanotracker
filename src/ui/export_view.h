// ui/export_view — the EXPORT window: output path, format and
// per-format knobs, order range, stems, post-processing, metadata and
// preset management over io/export_render. Ports the offline half of
// the web TrackerExportModal (Source/.../src/components/
// TrackerExportModal.tsx); the realtime-capture mode has no native
// counterpart (fixed offline render, FIXES.md #7).
//
// Threading: the render runs on a worker thread that owns value copies
// only (project, write extras, path, options) — it never touches the
// session, ImGui, or this view's form state. The UI thread launches it,
// polls `running_`, and consumes the mutex-guarded result once the
// thread finishes; the destructor joins an in-flight render.
#pragma once

#include "app/project_session.h"
#include "io/export_presets.h"
#include "io/export_render.h"
#include "ui/theme.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nt::ui {

class ExportView {
public:
    // `preset_file` — conventionally config_dir()/export_presets.json.
    explicit ExportView(std::filesystem::path preset_file);
    ~ExportView();

    ExportView(const ExportView&) = delete;
    ExportView& operator=(const ExportView&) = delete;
    ExportView(ExportView&&) = delete;
    ExportView& operator=(ExportView&&) = delete;

    // `open` follows the shell toggle contract (draw_help_window):
    // false skips drawing entirely; the window's close box clears it.
    void draw(app::ProjectSession& session, const Theme& theme, bool& open);

private:
    void draw_preset_row();
    void draw_output_section(const app::ProjectSession& session);
    void draw_format_section();
    void draw_range_section(const app::ProjectSession& session);
    void draw_stems_section(const app::ProjectSession& session);
    void draw_post_section();
    void draw_metadata_section();
    void draw_status_section(app::ProjectSession& session, const Theme& theme);

    // Marks the form dirty relative to the selected preset and fills
    // first-open defaults (path and title from the project name).
    void apply_preset(const io::ExportPreset& preset);
    void prefill_defaults(const app::ProjectSession& session);
    void set_path_extension_for_format();
    // The draft options plus the metadata text fields — what a preset
    // save or an export launch actually captures.
    [[nodiscard]] io::ExportOptions snapshot_options();

    // Worker lifecycle. launch_export snapshots everything on the UI
    // thread; poll_worker joins a finished thread and consumes the
    // result slot.
    void launch_export(app::ProjectSession& session);
    void poll_worker();

    io::ExportPresetStore presets_;
    std::string preset_id_;
    bool preset_dirty_ = false;

    io::ExportOptions options_;
    bool end_order_last_; // surfaces end_order = -1 ("last")

    bool defaults_filled_ = false;
    std::array<char, 512> path_buf_{};
    std::array<char, 64> preset_name_buf_{};
    std::array<char, 128> meta_title_{};
    std::array<char, 128> meta_artist_{};
    std::array<char, 128> meta_album_{};
    std::array<char, 32> meta_date_{};
    std::array<char, 256> meta_comment_{};

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex result_mutex_;
    io::ExportResult result_; // guarded by result_mutex_ until joined

    std::string status_; // UI thread only
    bool last_export_ok_ = false;
    std::vector<std::string> last_files_; // display names of what was written
};

} // namespace nt::ui
