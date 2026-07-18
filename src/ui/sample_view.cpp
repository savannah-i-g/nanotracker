#include "ui/sample_view.h"

#include "engine/tracker_engine.h"
#include "io/export_post.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

// SAMPLE_CATEGORIES from the web engine (trackerEngine.ts:41).
constexpr std::array<const char*, 8> kCategories = {"-",   "DRUMS", "BASS", "MELODY",
                                                    "PAD", "FX",    "LOOP", "OTHER"};

// Tightest zoom window: below this the per-column rendering carries no
// information and the px→frame mapping degenerates.
constexpr std::uint32_t kMinViewFrames = 16;

constexpr float kScrollbarHeight = 12.0F;

engine::TrackerSample* find_sample(engine::TrackerProject& project, int slot) {
    for (engine::TrackerSample& s : project.samples) {
        if (s.id == slot) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace

void SampleView::draw_slot_list(app::ProjectSession& session, const Theme& theme) {
    engine::TrackerProject& project = session.project();
    for (int slot = 1; slot <= engine::kMaxSamples; ++slot) {
        const engine::TrackerSample* sample = find_sample(project, slot);
        std::array<char, 48> label{};
        if (sample != nullptr) {
            std::snprintf(label.data(), label.size(), "%02X %-22s %s", slot, sample->name.c_str(),
                          sample->format.c_str());
        } else {
            std::snprintf(label.data(), label.size(), "%02X --", slot);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, sample != nullptr ? theme.text : theme.text_dim);
        if (ImGui::Selectable(label.data(), slot == selected_slot_)) {
            selected_slot_ = slot;
        }
        ImGui::PopStyleColor();
    }
}

void SampleView::draw_properties(app::ProjectSession& session, const Theme& theme) const {
    if (ImGui::Button("CLEAR")) {
        session.clear_slot(selected_slot_);
    }
    ImGui::SameLine();
    if (ImGui::Button("PREVIEW")) {
        // C-5 audition on the last channel (out of the sequencer's way).
        session.preview_note(session.project().channels - 1, selected_slot_, 61);
    }
    ImGui::SameLine();
    ImGui::TextColored(theme.text_dim, "load via SAMPLE BROWSER");

    engine::TrackerSample* sample = find_sample(session.project(), selected_slot_);
    if (sample == nullptr) {
        ImGui::TextColored(theme.text_dim, "empty slot");
        return;
    }

    engine::TrackerSample edited = *sample;
    bool changed = false;
    ImGui::SetNextItemWidth(140.0F);
    changed |= ImGui::SliderInt("volume", &edited.volume, 0, 64);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0F);
    changed |= ImGui::SliderInt("pan", &edited.pan, 0, 255);
    ImGui::SetNextItemWidth(90.0F);
    changed |= ImGui::InputInt("base note", &edited.base_note);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    changed |= ImGui::InputInt("finetune", &edited.finetune);
    int loop_start = static_cast<int>(edited.loop_start);
    int loop_length = static_cast<int>(edited.loop_length);
    ImGui::SetNextItemWidth(110.0F);
    changed |= ImGui::InputInt("loop start", &loop_start);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0F);
    changed |= ImGui::InputInt("loop len", &loop_length);
    ImGui::SetNextItemWidth(110.0F);
    if (ImGui::BeginCombo(
            "category", kCategories[static_cast<std::size_t>(std::clamp(edited.category, 0, 7))])) {
        for (int c = 0; c < static_cast<int>(kCategories.size()); ++c) {
            if (ImGui::Selectable(kCategories[static_cast<std::size_t>(c)], c == edited.category)) {
                edited.category = c;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (changed) {
        edited.base_note = std::clamp(edited.base_note, 0, 127);
        edited.finetune = std::clamp(edited.finetune, -128, 127);
        edited.loop_start = static_cast<std::uint32_t>(std::max(0, loop_start));
        edited.loop_length = static_cast<std::uint32_t>(std::max(0, loop_length));
        session.set_sample_meta(selected_slot_, edited);
    }

    ImGui::TextColored(theme.text_dim, "%u frames @ %u Hz (%s)", sample->frames,
                       sample->sample_rate, sample->format.c_str());
}

void SampleView::draw_op_toolbar(app::ProjectSession& session, const audio::SampleBuffer* buffer) {
    const bool have = buffer != nullptr && buffer->frames > 0;
    const std::uint32_t total = have ? buffer->frames : 0;
    const bool selection = have && has_selection();
    // Selection-or-whole: the session clamps end, so (0, UINT32_MAX)
    // is the documented whole-sample range.
    const std::uint32_t op_start = selection ? sel_min() : 0;
    const std::uint32_t op_end = selection ? sel_max() : UINT32_MAX;
    const io::FadeShape shape =
        fade_equal_power_ ? io::FadeShape::kEqualPower : io::FadeShape::kLinear;
    const auto report = [&](bool ok, const char* label) {
        status_ =
            ok ? std::string(label) + (selection ? " (selection)" : " (whole)") : session.error();
    };

    // ── View row ─────────────────────────────────────────────────────
    ImGui::BeginDisabled(!have);
    if (ImGui::Button("SEL ALL")) {
        sel_active_ = true;
        sel_a_ = 0;
        sel_b_ = total;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!selection);
    if (ImGui::Button("DESEL")) {
        sel_active_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("ZOOM SEL")) {
        view_frames_ = std::max(kMinViewFrames, sel_max() - sel_min());
        if (view_frames_ >= total) {
            view_frames_ = 0; // selection spans everything: unzoomed
        }
        view_start_ = std::min(sel_min(), total - view_length(total));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(view_frames_ == 0);
    if (ImGui::Button("FULL")) {
        view_start_ = 0;
        view_frames_ = 0;
    }
    ImGui::EndDisabled();

    // ── Edit row ─────────────────────────────────────────────────────
    ImGui::BeginDisabled(!selection);
    if (ImGui::Button("TRIM")) {
        report(session.sample_trim(selected_slot_, op_start, op_end), "trim");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("SILENCE")) {
        report(session.sample_silence(selected_slot_, op_start, op_end), "silence");
    }
    ImGui::SameLine();
    if (ImGui::Button("FADE IN")) {
        report(session.sample_fade_in(selected_slot_, op_start, op_end, shape), "fade in");
    }
    ImGui::SameLine();
    if (ImGui::Button("FADE OUT")) {
        report(session.sample_fade_out(selected_slot_, op_start, op_end, shape), "fade out");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(fade_equal_power_ ? "EQP" : "LIN")) {
        fade_equal_power_ = !fade_equal_power_;
    }
    ImGui::SetItemTooltip("fade shape: linear / equal-power");
    ImGui::SameLine();
    if (ImGui::Button("REVERSE")) {
        report(session.sample_reverse(selected_slot_, op_start, op_end), "reverse");
    }

    // ── Amplitude row ────────────────────────────────────────────────
    if (ImGui::Button("NORM")) {
        report(session.sample_normalize(selected_slot_, op_start, op_end, normalize_target_),
               "normalize");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0F);
    ImGui::SliderFloat("target", &normalize_target_, 0.0F, 1.0F, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("GAIN")) {
        report(session.sample_gain_db(selected_slot_, op_start, op_end, gain_db_), "gain");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0F);
    if (ImGui::InputFloat("dB", &gain_db_, 0.0F, 0.0F, "%.1f")) {
        gain_db_ = std::clamp(gain_db_, -24.0F, 24.0F);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selection);
    if (ImGui::Button("DC FIX")) {
        report(session.sample_remove_dc(selected_slot_), "remove DC");
    }
    ImGui::EndDisabled();
    if (selection && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("whole-sample only — deselect first");
    }
    ImGui::EndDisabled();
}

void SampleView::draw_readout(const audio::SampleBuffer* buffer, const Theme& theme) {
    if (buffer != nullptr && has_selection()) {
        const std::uint32_t len = sel_max() - sel_min();
        const double seconds =
            buffer->rate > 0 ? static_cast<double>(len) / static_cast<double>(buffer->rate) : 0.0;
        ImGui::TextColored(theme.text_dim, "sel %u..%u  %u fr  %.3f s", sel_min(), sel_max(), len,
                           seconds);
    } else {
        ImGui::TextColored(theme.text_dim, "sel --");
    }
    if (buffer != nullptr && view_frames_ != 0) {
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim, "  view x%.1f",
                           static_cast<double>(buffer->frames) /
                               static_cast<double>(view_length(buffer->frames)));
    }
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim, " | %s", status_.c_str());
    }
}

void SampleView::reconcile_view_state(const audio::SampleBuffer* buffer) {
    const std::uint32_t frames = buffer != nullptr ? buffer->frames : 0;
    if (selected_slot_ != seen_slot_ || frames == 0) {
        // Different buffer entirely (or none): view state is meaningless.
        sel_active_ = false;
        drag_selecting_ = false;
        view_start_ = 0;
        view_frames_ = 0;
        if (selected_slot_ != seen_slot_) {
            status_.clear();
        }
    } else if (frames != seen_frames_) {
        // Same slot, new length (trim/undo): clamp what survives.
        sel_a_ = std::min(sel_a_, frames);
        sel_b_ = std::min(sel_b_, frames);
        if (sel_a_ == sel_b_) {
            sel_active_ = false;
        }
        if (view_frames_ != 0) {
            view_frames_ = std::min(view_frames_, frames);
            if (view_frames_ == frames) {
                view_frames_ = 0;
            }
        }
        view_start_ = std::min(view_start_, frames - view_length(frames));
    }
    seen_slot_ = selected_slot_;
    seen_frames_ = frames;
}

void SampleView::draw_waveform(app::ProjectSession& session, const Theme& theme) {
    const audio::SampleBuffer* buffer = session.sample_buffer(selected_slot_);
    const engine::TrackerSample* sample = find_sample(session.project(), selected_slot_);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::max(8.0F, avail.x);
    const float height =
        std::max(60.0F, avail.y - kScrollbarHeight - ImGui::GetStyle().ItemSpacing.y);

    const bool have = buffer != nullptr && buffer->frames > 1;
    const std::uint32_t total = have ? buffer->frames : 0;
    // Defensive clamps on top of reconcile_view_state — indexing below
    // must hold even if view state and buffer ever disagree.
    const std::uint32_t view_len = have ? std::min(view_length(total), total) : 0;
    const std::uint32_t view_start = have ? std::min(view_start_, total - view_len) : 0;
    const float inner_w = width - 2.0F;

    // The invisible button both reserves the layout space and owns the
    // mouse: drag = selection, plain click = deselect, wheel = scroll,
    // ctrl+wheel = zoom about the cursor (web WaveformEditor semantics).
    ImGui::InvisibleButton("wave", ImVec2(width, height));
    const auto frame_at = [&](float x) -> std::uint32_t {
        const double t = std::clamp(
            static_cast<double>(x - origin.x - 1.0F) / static_cast<double>(inner_w), 0.0, 1.0);
        const auto offset = static_cast<std::uint32_t>(std::lround(t * view_len));
        return std::min(total, view_start + offset);
    };
    if (have) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActivated()) {
            drag_anchor_ = frame_at(io.MousePos.x);
            drag_selecting_ = false;
        }
        if (ImGui::IsItemActive()) {
            if (!drag_selecting_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0F)) {
                drag_selecting_ = true;
            }
            if (drag_selecting_) {
                sel_active_ = true;
                sel_a_ = drag_anchor_;
                sel_b_ = frame_at(io.MousePos.x);
            }
        }
        if (ImGui::IsItemDeactivated()) {
            if (!drag_selecting_) {
                sel_active_ = false; // plain click: deselect
            }
            drag_selecting_ = false;
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0F) {
            if (io.KeyCtrl) {
                // Zoom keeping the frame under the cursor stationary.
                const double factor = io.MouseWheel > 0.0F ? 1.0 / 1.3 : 1.3;
                const auto min_len = std::min<long long>(kMinViewFrames, total);
                const auto new_len = static_cast<std::uint32_t>(
                    std::clamp(std::llround(static_cast<double>(view_len) * factor), min_len,
                               static_cast<long long>(total)));
                const std::uint32_t anchor = frame_at(io.MousePos.x);
                const double frac =
                    std::clamp(static_cast<double>(io.MousePos.x - origin.x - 1.0F) /
                                   static_cast<double>(inner_w),
                               0.0, 1.0);
                const auto lead =
                    static_cast<std::uint32_t>(std::lround(frac * static_cast<double>(new_len)));
                view_start_ = std::min(anchor > lead ? anchor - lead : 0, total - new_len);
                view_frames_ = new_len == total ? 0 : new_len;
            } else if (view_frames_ != 0) {
                const auto step = std::max<std::uint32_t>(1, view_len / 10);
                if (io.MouseWheel > 0.0F) {
                    view_start_ = view_start > step ? view_start - step : 0;
                } else {
                    view_start_ = std::min(view_start + step, total - view_len);
                }
            }
        }
    }

    draw->AddRectFilled({origin.x, origin.y}, {origin.x + width, origin.y + height},
                        ImGui::GetColorU32(theme.background_alt));
    draw->AddRect({origin.x, origin.y}, {origin.x + width, origin.y + height},
                  ImGui::GetColorU32(theme.border));

    if (have && width > 4) {
        const float mid = origin.y + (height / 2);
        const auto columns = static_cast<int>(width) - 2;
        const double frames_per_col = static_cast<double>(view_len) / std::max(1, columns);
        if (frames_per_col >= 1.0) {
            // Zoomed out: min/max envelope per pixel column.
            for (int x = 0; x < columns; ++x) {
                const auto begin = static_cast<std::size_t>(view_start) +
                                   static_cast<std::size_t>(x * frames_per_col);
                const auto end =
                    std::min<std::size_t>(static_cast<std::size_t>(view_start) +
                                              static_cast<std::size_t>((x + 1) * frames_per_col),
                                          total);
                float lo = 0.0F;
                float hi = 0.0F;
                for (std::size_t f = begin; f < end; ++f) {
                    const float v = buffer->interleaved[f * 2];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                const float px = origin.x + 1 + static_cast<float>(x);
                draw->AddLine({px, mid - (hi * height * 0.48F)}, {px, mid - (lo * height * 0.48F)},
                              ImGui::GetColorU32(theme.primary));
            }
        } else {
            // Zoomed in past one frame per column: nearest-sample polyline.
            ImVec2 prev{};
            for (int x = 0; x < columns; ++x) {
                const auto f = std::min<std::size_t>(
                    static_cast<std::size_t>(view_start) +
                        static_cast<std::size_t>(std::lround(x * frames_per_col)),
                    static_cast<std::size_t>(total) - 1);
                const float v = buffer->interleaved[f * 2];
                const ImVec2 point{origin.x + 1 + static_cast<float>(x),
                                   mid - (v * height * 0.48F)};
                if (x > 0) {
                    draw->AddLine(prev, point, ImGui::GetColorU32(theme.primary));
                }
                prev = point;
            }
        }

        // Frame → pixel within the visible window, clamped to the rect.
        const auto frame_to_px = [&](std::uint32_t frame) -> float {
            const double t = (static_cast<double>(frame) - static_cast<double>(view_start)) /
                             static_cast<double>(view_len);
            return origin.x + 1 +
                   static_cast<float>(std::clamp(t, 0.0, 1.0) * static_cast<double>(inner_w));
        };

        // Selection overlay: neutral text-colour shading so it reads
        // apart from the primary-coloured loop region.
        if (has_selection() && sel_min() < view_start + view_len && sel_max() > view_start) {
            const float sx = frame_to_px(sel_min());
            const float ex = frame_to_px(sel_max());
            ImVec4 fill = theme.text;
            fill.w = 0.15F;
            ImVec4 edge = theme.text;
            edge.w = 0.8F;
            draw->AddRectFilled({sx, origin.y}, {ex, origin.y + height}, ImGui::GetColorU32(fill));
            draw->AddLine({sx, origin.y}, {sx, origin.y + height}, ImGui::GetColorU32(edge));
            draw->AddLine({ex, origin.y}, {ex, origin.y + height}, ImGui::GetColorU32(edge));
        }

        // Loop region overlay (converted from source-rate frames).
        if (sample != nullptr && sample->loop_length > 0 && sample->frames > 0) {
            const double scale =
                static_cast<double>(total) / std::max<std::uint32_t>(1, sample->frames);
            const auto loop_begin =
                static_cast<std::uint32_t>(static_cast<double>(sample->loop_start) * scale);
            const auto loop_end = static_cast<std::uint32_t>(
                static_cast<double>(sample->loop_start + sample->loop_length) * scale);
            if (loop_begin < view_start + view_len && loop_end > view_start) {
                const float lx = frame_to_px(loop_begin);
                const float rx = frame_to_px(loop_end);
                // The dark-theme glow (0.08 on light) is invisible over a
                // bright waveform pane; a light theme needs a firmer fill.
                // Dark themes keep primary_glow exactly (same rgb+alpha).
                ImVec4 loop_fill = theme.primary;
                loop_fill.w = theme.light ? 0.22F : theme.primary_glow.w;
                draw->AddRectFilled({lx, origin.y}, {rx, origin.y + height},
                                    ImGui::GetColorU32(loop_fill));
                draw->AddLine({lx, origin.y}, {lx, origin.y + height},
                              ImGui::GetColorU32(theme.primary));
                draw->AddLine({rx, origin.y}, {rx, origin.y + height},
                              ImGui::GetColorU32(theme.primary));
            }
        }
    } else {
        draw->AddText({origin.x + 8, origin.y + 8}, ImGui::GetColorU32(theme.text_dim),
                      "no sample data");
    }

    draw_view_scrollbar(buffer, theme);
}

void SampleView::draw_view_scrollbar(const audio::SampleBuffer* buffer, const Theme& theme) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(8.0F, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("wavescroll", ImVec2(width, kScrollbarHeight));

    const bool zoomed = buffer != nullptr && view_frames_ != 0 && view_frames_ < buffer->frames;
    draw->AddRectFilled({origin.x, origin.y}, {origin.x + width, origin.y + kScrollbarHeight},
                        ImGui::GetColorU32(theme.background_alt));
    draw->AddRect({origin.x, origin.y}, {origin.x + width, origin.y + kScrollbarHeight},
                  ImGui::GetColorU32(theme.border));
    if (!zoomed) {
        return;
    }

    const std::uint32_t total = buffer->frames;
    const std::uint32_t view_len = std::min(view_length(total), total);
    if (ImGui::IsItemActive()) {
        // Click-to-centre + drag, like the web editor's scrollbar.
        const double frac = std::clamp(static_cast<double>(ImGui::GetIO().MousePos.x - origin.x) /
                                           static_cast<double>(width),
                                       0.0, 1.0);
        const auto centre = static_cast<std::uint32_t>(frac * static_cast<double>(total));
        const std::uint32_t half = view_len / 2;
        view_start_ = std::min(centre > half ? centre - half : 0, total - view_len);
    }

    const double per_frame = static_cast<double>(width) / static_cast<double>(total);
    const float thumb_x =
        origin.x + static_cast<float>(static_cast<double>(view_start_) * per_frame);
    const float thumb_w =
        std::max(8.0F, static_cast<float>(static_cast<double>(view_len) * per_frame));
    draw->AddRectFilled(
        {thumb_x, origin.y + 2},
        {std::min(thumb_x + thumb_w, origin.x + width), origin.y + kScrollbarHeight - 2},
        ImGui::GetColorU32(theme.text_dim));
}

void SampleView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("SAMPLES")) {
        // Same undo shortcuts as the pattern editor when this window is
        // focused (text inputs keep their own editing keys).
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput &&
            io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                session.undo().undo();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                session.undo().redo();
            }
        }

        if (ImGui::BeginChild("slots", ImVec2(ImGui::CalcTextSize("0").x * 34.0F, 0),
                              ImGuiChildFlags_Borders)) {
            draw_slot_list(session, theme);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            const audio::SampleBuffer* buffer = session.sample_buffer(selected_slot_);
            reconcile_view_state(buffer);
            draw_properties(session, theme);
            ImGui::Separator();
            draw_op_toolbar(session, buffer);
            // Ops swap the resident buffer (the old one retires behind
            // the reclamation fence, staying readable this frame) —
            // re-fetch and re-reconcile before anything indexes it.
            buffer = session.sample_buffer(selected_slot_);
            reconcile_view_state(buffer);
            draw_readout(buffer, theme);
            draw_waveform(session, theme);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace nt::ui
