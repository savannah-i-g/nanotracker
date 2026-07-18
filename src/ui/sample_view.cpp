#include "ui/sample_view.h"

#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>

namespace nt::ui {

namespace {

// SAMPLE_CATEGORIES from the web engine (trackerEngine.ts:41).
constexpr std::array<const char*, 8> kCategories = {"-",   "DRUMS", "BASS", "MELODY",
                                                    "PAD", "FX",    "LOOP", "OTHER"};

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

void SampleView::draw_properties(app::ProjectSession& session, const Theme& theme) {
    ImGui::InputTextWithHint("##spath", "path/to/sample.{wav,ogg,mp3}", path_buf_.data(),
                             path_buf_.size());
    ImGui::SameLine();
    if (ImGui::Button("LOAD")) {
        status_ = session.load_sample_into_slot(selected_slot_, path_buf_.data()) ? "loaded"
                                                                                  : session.error();
    }
    ImGui::SameLine();
    if (ImGui::Button("CLEAR")) {
        session.clear_slot(selected_slot_);
    }
    ImGui::SameLine();
    if (ImGui::Button("PREVIEW")) {
        // C-5 audition on the last channel (out of the sequencer's way).
        session.preview_note(session.project().channels - 1, selected_slot_, 61);
    }
    if (!status_.empty()) {
        ImGui::TextColored(theme.text_dim, "%s", status_.c_str());
    }

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

void SampleView::draw_waveform(app::ProjectSession& session, const Theme& theme) const {
    const audio::SampleBuffer* buffer = session.sample_buffer(selected_slot_);
    const engine::TrackerSample* sample = find_sample(session.project(), selected_slot_);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = std::max(60.0F, avail.y);
    draw->AddRectFilled({origin.x, origin.y}, {origin.x + avail.x, origin.y + height},
                        ImGui::GetColorU32(theme.background_alt));
    draw->AddRect({origin.x, origin.y}, {origin.x + avail.x, origin.y + height},
                  ImGui::GetColorU32(theme.border));

    if (buffer != nullptr && buffer->frames > 1 && avail.x > 4) {
        const float mid = origin.y + (height / 2);
        const auto columns = static_cast<int>(avail.x) - 2;
        const double frames_per_col = static_cast<double>(buffer->frames) / std::max(1, columns);
        for (int x = 0; x < columns; ++x) {
            const auto begin = static_cast<std::size_t>(x * frames_per_col);
            const auto end = static_cast<std::size_t>(
                std::min<double>((x + 1) * frames_per_col, buffer->frames));
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

        // Loop region overlay (converted from source-rate frames).
        if (sample != nullptr && sample->loop_length > 0 && sample->sample_rate > 0) {
            const double scale =
                static_cast<double>(buffer->frames) / std::max<std::uint32_t>(1, sample->frames);
            const double per_frame = avail.x / static_cast<double>(buffer->frames);
            const float lx = origin.x + static_cast<float>(sample->loop_start * scale * per_frame);
            const float rx =
                origin.x +
                static_cast<float>((sample->loop_start + sample->loop_length) * scale * per_frame);
            draw->AddRectFilled({lx, origin.y}, {rx, origin.y + height},
                                ImGui::GetColorU32(theme.primary_glow));
            draw->AddLine({lx, origin.y}, {lx, origin.y + height},
                          ImGui::GetColorU32(theme.primary));
            draw->AddLine({rx, origin.y}, {rx, origin.y + height},
                          ImGui::GetColorU32(theme.primary));
        }
    } else {
        draw->AddText({origin.x + 8, origin.y + 8}, ImGui::GetColorU32(theme.text_dim),
                      "no sample data");
    }
    ImGui::Dummy(ImVec2(avail.x, height));
}

void SampleView::draw(app::ProjectSession& session, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("SAMPLES")) {
        if (ImGui::BeginChild("slots", ImVec2(ImGui::CalcTextSize("0").x * 34.0F, 0),
                              ImGuiChildFlags_Borders)) {
            draw_slot_list(session, theme);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            draw_properties(session, theme);
            ImGui::Separator();
            draw_waveform(session, theme);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace nt::ui
