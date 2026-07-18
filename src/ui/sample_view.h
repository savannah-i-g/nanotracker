// ui/sample_view — the SAMPLES window: 31 slots, per-slot clear/
// preview, sample properties (volume/pan/base note/finetune/loops/
// category), and the waveform editor: drag selection, zoom-to-selection
// with horizontal scroll, and the destructive-op toolbar routed through
// app/project_session (half-open device-rate frame ranges; whole sample
// when nothing is selected). Ports the web WaveformEditor's semantics
// (Source/.../src/components/WaveformEditor.tsx) onto native
// interactions; loading files is the SAMPLE BROWSER window's job
// (ui/sample_browser_view).
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <cstdint>
#include <string>

namespace nt::ui {

class SampleView {
public:
    void draw(app::ProjectSession& session, const Theme& theme);

    [[nodiscard]] int selected_slot() const { return selected_slot_; }

private:
    void draw_slot_list(app::ProjectSession& session, const Theme& theme);
    void draw_properties(app::ProjectSession& session, const Theme& theme) const;
    void draw_op_toolbar(app::ProjectSession& session, const audio::SampleBuffer* buffer);
    void draw_readout(const audio::SampleBuffer* buffer, const Theme& theme);
    void draw_waveform(app::ProjectSession& session, const Theme& theme);
    void draw_view_scrollbar(const audio::SampleBuffer* buffer, const Theme& theme);

    // Selection helpers. The selection lives in device-rate frames of
    // the resident buffer as a half-open [sel_min, sel_max) range —
    // exactly what the session's destructive ops take.
    [[nodiscard]] bool has_selection() const { return sel_active_ && sel_a_ != sel_b_; }

    [[nodiscard]] std::uint32_t sel_min() const { return sel_a_ < sel_b_ ? sel_a_ : sel_b_; }

    [[nodiscard]] std::uint32_t sel_max() const { return sel_a_ < sel_b_ ? sel_b_ : sel_a_; }

    // Visible window length in frames (0 stored in view_frames_ means
    // "whole buffer").
    [[nodiscard]] std::uint32_t view_length(std::uint32_t total) const {
        return view_frames_ == 0 ? total : view_frames_;
    }

    // Reconciles selection/view state against the resident buffer:
    // slot switches drop them, length changes (trim/undo) clamp them
    // (cleared when nothing survives the clamp).
    void reconcile_view_state(const audio::SampleBuffer* buffer);

    int selected_slot_ = 1;
    std::string status_;

    // Selection endpoints (unordered; sel_min/sel_max order them).
    bool sel_active_ = false;
    std::uint32_t sel_a_ = 0;
    std::uint32_t sel_b_ = 0;
    // Drag-in-progress state for the waveform's invisible button.
    bool drag_selecting_ = false;
    std::uint32_t drag_anchor_ = 0;

    // Zoom window over the buffer; view_frames_ == 0 means unzoomed.
    std::uint32_t view_start_ = 0;
    std::uint32_t view_frames_ = 0;

    // Buffer identity observed last frame (reconcile_view_state).
    int seen_slot_ = -1;
    std::uint32_t seen_frames_ = 0;

    // Op-toolbar parameters.
    bool fade_equal_power_ = false; // false = linear
    float normalize_target_ = 1.0F;
    float gain_db_ = 0.0F;
};

} // namespace nt::ui
