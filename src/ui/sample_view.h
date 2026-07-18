// ui/sample_view — the SAMPLES window: 31 slots, per-slot load/clear/
// preview, sample properties (volume/pan/base note/finetune/loops/
// category), and a waveform display with the loop region marked.
// Ports the essentials of the web sample list + WaveformEditor
// (Source/.../src/components/SampleBrowserWindow.tsx,
// WaveformEditor.tsx); destructive waveform edit operations are the
// waveform stage of the instrument editor and arrive with them.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <array>

namespace nt::ui {

class SampleView {
public:
    void draw(app::ProjectSession& session, const Theme& theme);

    [[nodiscard]] int selected_slot() const { return selected_slot_; }

private:
    void draw_slot_list(app::ProjectSession& session, const Theme& theme);
    void draw_properties(app::ProjectSession& session, const Theme& theme);
    void draw_waveform(app::ProjectSession& session, const Theme& theme) const;

    int selected_slot_ = 1;
    std::array<char, 512> path_buf_{};
    std::string status_;
};

} // namespace nt::ui
