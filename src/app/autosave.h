// app/autosave — rotating autosave slots and the crash journal. Every
// interval the open project writes to the next of three slots under
// the config directory (in-process plugins can take the host down —
// 08-external-plugins.md's crash posture). A session lock file marks
// a run in progress; finding one at startup means the previous run
// did not exit cleanly, and the UI offers the newest autosave.
#pragma once

#include "app/project_session.h"

#include <filesystem>
#include <string>

namespace nt::app {

class Autosave {
public:
    // `dir` — e.g. config_dir()/autosave. Created on first save.
    explicit Autosave(std::filesystem::path dir);

    // True when the previous run left its lock behind (crash / kill).
    [[nodiscard]] bool previous_run_crashed() const { return previous_crashed_; }

    // Newest autosave slot, empty when none exist.
    [[nodiscard]] std::filesystem::path latest_slot() const;

    // Call once per frame; saves when the interval elapses.
    void update(ProjectSession& session, double now_seconds);

    // Clean-exit marker removal (call on the normal shutdown path).
    void end_session();

    static constexpr double kIntervalSeconds = 120.0;
    static constexpr int kSlots = 3;

private:
    std::filesystem::path dir_;
    std::filesystem::path lock_path_;
    bool previous_crashed_ = false;
    double last_save_ = 0.0;
    int next_slot_ = 0;
};

} // namespace nt::app
