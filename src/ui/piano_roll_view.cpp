#include "ui/piano_roll_view.h"

#include "engine/sequence_ops.h"
#include "engine/tracker_engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <numeric>

namespace nt::ui {

namespace {

constexpr int kPitchRows = 36; // three octaves visible
constexpr float kRowHeight = 12.0F;
constexpr float kTickWidth = 3.0F;
constexpr float kResizeGripPx = 4.0F; // right-edge resize hot zone
constexpr float kClickSlopPx = 3.0F;  // marquee under this is a click

bool is_black_key(int pitch) {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

const char* note_name(int pitch) {
    static const std::array<const char*, 12> kNames = {"C-", "C#", "D-", "D#", "E-", "F-",
                                                       "F#", "G-", "G#", "A-", "A#", "B-"};
    return kNames[static_cast<std::size_t>(pitch % 12)];
}

} // namespace

bool PianoRollView::is_selected(int note_index) const {
    return std::binary_search(selection_.begin(), selection_.end(), note_index);
}

void PianoRollView::toggle_selected(int note_index) {
    const auto at = std::lower_bound(selection_.begin(), selection_.end(), note_index);
    if (at != selection_.end() && *at == note_index) {
        selection_.erase(at);
    } else {
        selection_.insert(at, note_index);
    }
}

// Batch note replacement: one stop→swap→publish through the session
// (seq_replace_notes). The tagged input is sorted here first so
// selection indices can be recomputed against the exact order the
// layer will hold.
void PianoRollView::replace_notes(app::ProjectSession& session, int pattern_index,
                                  std::vector<TaggedNote> tagged) {
    std::stable_sort(tagged.begin(), tagged.end(), [](const TaggedNote& a, const TaggedNote& b) {
        return a.note.start_tick < b.note.start_tick;
    });
    selection_.clear();
    std::vector<engine::SequenceNote> notes;
    notes.reserve(tagged.size());
    for (std::size_t n = 0; n < tagged.size(); ++n) {
        if (tagged[n].selected) {
            selection_.push_back(static_cast<int>(n));
        }
        notes.push_back(tagged[n].note);
    }
    session.seq_replace_notes(pattern_index, channel_, layer_, std::move(notes));
}

void PianoRollView::apply_transform(app::ProjectSession& session, int pattern_index,
                                    const engine::SequenceLayer& layer,
                                    const Transform& transform) {
    if (layer.notes.empty()) {
        return;
    }
    // Web toolbox convention: no selection means every note.
    std::vector<int> indices = selection_;
    if (indices.empty()) {
        indices.resize(layer.notes.size());
        std::iota(indices.begin(), indices.end(), 0);
    }
    std::vector<engine::SequenceNote> input;
    input.reserve(indices.size());
    for (const int index : indices) {
        input.push_back(layer.notes[static_cast<std::size_t>(index)]);
    }
    const std::vector<engine::SequenceNote> output = transform(input);
    std::vector<TaggedNote> tagged;
    tagged.reserve(layer.notes.size() - indices.size() + output.size());
    std::size_t next = 0; // walks `indices` (ascending)
    for (std::size_t n = 0; n < layer.notes.size(); ++n) {
        if (next < indices.size() && indices[next] == static_cast<int>(n)) {
            ++next;
            continue;
        }
        tagged.push_back({.note = layer.notes[n], .selected = false});
    }
    for (const engine::SequenceNote& note : output) {
        tagged.push_back({.note = note, .selected = true});
    }
    replace_notes(session, pattern_index, std::move(tagged));
}

void PianoRollView::draw_toolbox(app::ProjectSession& session, int pattern_index,
                                 const engine::SequenceLayer* layer, int ticks_per_row) {
    const bool have_notes = layer != nullptr && !layer->notes.empty();
    Transform tool;
    ImGui::BeginDisabled(!have_notes);

    // Fixed transforms (web toolbox row: quantize/humanize/transpose/
    // reverse/invert).
    ImGui::TextDisabled("tools");
    ImGui::SameLine();
    if (ImGui::SmallButton("q1")) {
        tool = [ticks_per_row](const std::vector<engine::SequenceNote>& in) {
            return engine::quantize(in, ticks_per_row, 1, 100);
        };
    }
    ImGui::SetItemTooltip("quantize to rows");
    ImGui::SameLine();
    if (ImGui::SmallButton("q1/2")) {
        tool = [ticks_per_row](const std::vector<engine::SequenceNote>& in) {
            return engine::quantize(in, ticks_per_row, 2, 100);
        };
    }
    ImGui::SetItemTooltip("quantize to half rows");
    ImGui::SameLine();
    if (ImGui::SmallButton("hum")) {
        tool = [this, ticks_per_row](const std::vector<engine::SequenceNote>& in) {
            return engine::humanize(in, ticks_per_row, rng_);
        };
    }
    ImGui::SetItemTooltip("humanize timing / velocity / length");
    ImGui::SameLine();
    if (ImGui::SmallButton("-12")) {
        tool = [](const std::vector<engine::SequenceNote>& in) {
            return engine::transpose(in, -12);
        };
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+7")) {
        tool = [](const std::vector<engine::SequenceNote>& in) { return engine::transpose(in, 7); };
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+12")) {
        tool = [](const std::vector<engine::SequenceNote>& in) {
            return engine::transpose(in, 12);
        };
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("rev")) {
        tool = [](const std::vector<engine::SequenceNote>& in) { return engine::reverse(in); };
    }
    ImGui::SetItemTooltip("reverse in time");
    ImGui::SameLine();
    if (ImGui::SmallButton("inv")) {
        tool = [](const std::vector<engine::SequenceNote>& in) { return engine::invert(in); };
    }
    ImGui::SetItemTooltip("mirror around the pitch center");

    // Parameterised tools: one apply button each over shared controls
    // (the web spreads these across seven menu entries).
    ImGui::SetNextItemWidth(80.0F);
    ImGui::Combo("##arpdir", &arp_direction_, "up\0down\0up/down\0random\0");
    ImGui::SameLine();
    ImGui::RadioButton("r4", &arp_rate_, 4);
    ImGui::SameLine();
    ImGui::RadioButton("r8", &arp_rate_, 8);
    ImGui::SameLine();
    ImGui::RadioButton("o1", &arp_octaves_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("o2", &arp_octaves_, 2);
    ImGui::SameLine();
    if (ImGui::SmallButton("arp")) {
        const auto direction = static_cast<engine::ArpDirection>(arp_direction_);
        const int rate = arp_rate_;
        const int octaves = arp_octaves_;
        tool = [this, ticks_per_row, direction, rate,
                octaves](const std::vector<engine::SequenceNote>& in) {
            return engine::arpeggiate(in, ticks_per_row, direction, rate, rng_, 80, octaves);
        };
    }
    ImGui::SetItemTooltip("replace the selection with an arpeggiated run");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0F);
    ImGui::Combo("##velshape", &velocity_shape_, "crescendo\0decrescendo\0accent\0flat\0");
    ImGui::SameLine();
    if (ImGui::SmallButton("vel")) {
        const auto shape = static_cast<engine::VelocityCurveShape>(velocity_shape_);
        tool = [shape](const std::vector<engine::SequenceNote>& in) {
            // Flat pins to 80, matching the web's "Flatten Vel".
            return shape == engine::VelocityCurveShape::kFlat
                       ? engine::velocity_curve(in, shape, 80, 80)
                       : engine::velocity_curve(in, shape);
        };
    }
    ImGui::SetItemTooltip("shape velocities across the selection");
    ImGui::SameLine();
    if (ImGui::SmallButton("g50")) {
        tool = [](const std::vector<engine::SequenceNote>& in) {
            return engine::gate_length(in, 50);
        };
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("g75")) {
        tool = [](const std::vector<engine::SequenceNote>& in) {
            return engine::gate_length(in, 75);
        };
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("g100")) {
        tool = [](const std::vector<engine::SequenceNote>& in) {
            return engine::gate_length(in, 100);
        };
    }
    ImGui::SetItemTooltip("gate length (100 = legato)");
    ImGui::EndDisabled();

    if (tool && layer != nullptr) {
        apply_transform(session, pattern_index, *layer, tool);
    }
}

void PianoRollView::audition_pitch(app::ProjectSession& session, const engine::SequenceLayer* layer,
                                   int pitch) const {
    const int slot = layer != nullptr && layer->instrument > 0 ? layer->instrument : 1;
    const auto& table = session.project().instrument_table;
    const bool is_plugin =
        slot >= 1 && slot <= static_cast<int>(table.size()) &&
        table[static_cast<std::size_t>(slot - 1)].type != engine::InstrumentSourceType::kSample;
    if (is_plugin) {
        session.preview_plugin_note(slot, pitch, 0.9F);
    } else {
        session.preview_note(channel_, slot, pitch - 11); // MIDI → tracker note
    }
}

void PianoRollView::draw_toolbar(app::ProjectSession& session, io::Settings& settings,
                                 int pattern_index, const Theme& theme) {
    const engine::TrackerProject& project = session.project();

    // ── Identity, mode, audition, transport ──────────────────────────
    // Channel/layer identity stays visible up here (the window title is
    // shared by the dock tab); the mode toggle mirrors the pattern
    // grid's select-vs-edit split.
    ImGui::TextColored(theme.primary, "CH%d  L%d", channel_ + 1, layer_);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    int mode = static_cast<int>(mode_);
    ImGui::RadioButton("select", &mode, static_cast<int>(Mode::kSelect));
    ImGui::SetItemTooltip("marquee select; drag a note to move, its right edge to resize");
    ImGui::SameLine();
    ImGui::RadioButton("add", &mode, static_cast<int>(Mode::kAdd));
    ImGui::SetItemTooltip("click adds a note; drag sets its length; right-click removes");
    mode_ = static_cast<Mode>(mode);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox("audition", &settings.piano_roll_audition);
    ImGui::SetItemTooltip("preview added notes through the layer instrument");
    ImGui::SameLine();
    if (ImGui::SmallButton("play from start")) {
        session.play();
    }
    // The session exposes only a from-the-top transport start (order 0);
    // row-accurate "play from here" is not wired for the piano roll yet.
    ImGui::SetItemTooltip("start the transport (F5 in the grid also toggles)");

    // ── Layer selector: note presence + "+ layer" ────────────────────
    const auto select_layer = [this](int target) {
        if (target != layer_) {
            layer_ = target;
            selection_.clear();
            drag_ = Drag::kNone;
        }
    };
    ImGui::TextDisabled("layers");
    int first_empty = -1;
    for (int l = 0; l < engine::kMaxSeqLayersPerChannel; ++l) {
        const engine::SequenceLayer* candidate =
            session.seq_layer(pattern_index, channel_, l, false);
        const bool has_notes = candidate != nullptr && !candidate->notes.empty();
        if (!has_notes && first_empty < 0) {
            first_empty = l;
        }
        ImGui::SameLine();
        // '*' marks lanes carrying notes; colour tracks the selection.
        std::array<char, 16> label{};
        std::snprintf(label.data(), label.size(), "%s%d##layer%d", has_notes ? "*" : "", l, l);
        ImVec4 tint = theme.text_dim;
        if (l == layer_) {
            tint = theme.primary;
        } else if (has_notes) {
            tint = theme.text;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, tint);
        if (ImGui::SmallButton(label.data())) {
            select_layer(l);
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(has_notes ? "layer %d (has notes)" : "layer %d (empty)", l);
    }
    ImGui::SameLine();
    // "+ layer": jump to the first empty lane (created implicitly on the
    // first note); all four in use → nothing to add.
    ImGui::BeginDisabled(first_empty < 0);
    if (ImGui::SmallButton("+ layer")) {
        select_layer(first_empty);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("switch to the next empty layer");

    // ── Channel, instrument, enable, grid pitch base ─────────────────
    ImGui::SetNextItemWidth(90.0F);
    int channel = channel_;
    if (ImGui::SliderInt("channel", &channel, 0, project.channels - 1)) {
        channel_ = channel;
        selection_.clear(); // indices are per-layer
        drag_ = Drag::kNone;
    }
    const engine::SequenceLayer* current =
        session.seq_layer(pattern_index, channel_, layer_, false);
    int instrument = current != nullptr ? current->instrument : 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0F);
    if (ImGui::InputInt("ins", &instrument)) {
        session.seq_set_layer_instrument(pattern_index, channel_, layer_,
                                         std::clamp(instrument, 0, engine::kMaxSamples));
    }
    bool enabled = current == nullptr || current->enabled;
    ImGui::SameLine();
    if (ImGui::Checkbox("on", &enabled)) {
        session.seq_set_layer_enabled(pattern_index, channel_, layer_, enabled);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0F);
    ImGui::SliderInt("octave", &base_pitch_, 24, 72, "base %d");
}

void PianoRollView::draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme) {
    ImGui::SetNextWindowPos(ImVec2{60, 60}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2{720, 620}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("piano roll")) {
        ImGui::End();
        return;
    }
    const engine::TrackerProject& project = session.project();
    const ImGuiIO& io = ImGui::GetIO();
    const int pattern_index = 0; // follows the pattern editor later
    channel_ = std::clamp(channel_, 0, project.channels - 1);

    draw_toolbar(session, settings, pattern_index, theme);
    engine::SequenceLayer* layer = session.seq_layer(pattern_index, channel_, layer_, false);

    const int rows = project.rows_per_pattern;
    const int ticks_per_row = std::max(1, project.speed);
    const int total_ticks = rows * ticks_per_row;

    draw_toolbox(session, pattern_index, layer, ticks_per_row);

    // ── Note grid ────────────────────────────────────────────────────
    const float grid_width = static_cast<float>(total_ticks) * kTickWidth;
    const float grid_height = kPitchRows * kRowHeight;

    ImGui::BeginChild("grid", ImVec2{0, grid_height + 16.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float scroll_x = ImGui::GetScrollX();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Stale-index guard: loads and external edits can shrink the layer
    // while selection indices persist across frames.
    const int note_count = layer != nullptr ? static_cast<int>(layer->notes.size()) : 0;
    std::erase_if(selection_, [note_count](int index) { return index >= note_count; });
    if (resize_note_ >= note_count) {
        resize_note_ = -1;
        if (drag_ == Drag::kResize) {
            drag_ = Drag::kNone;
        }
    }

    // Row shading + line grid.
    for (int p = 0; p < kPitchRows; ++p) {
        const int pitch = base_pitch_ + (kPitchRows - 1 - p);
        const float y = origin.y + (static_cast<float>(p) * kRowHeight);
        if (is_black_key(pitch)) {
            draw->AddRectFilled(ImVec2{origin.x, y}, ImVec2{origin.x + grid_width, y + kRowHeight},
                                ImGui::GetColorU32(theme.background_alt));
        }
    }
    for (int r = 0; r <= rows; ++r) {
        const float x = origin.x + (static_cast<float>(r * ticks_per_row) * kTickWidth);
        const bool major = r % 4 == 0;
        draw->AddLine(ImVec2{x, origin.y}, ImVec2{x, origin.y + grid_height},
                      ImGui::GetColorU32(major ? theme.border : theme.background_alt));
    }
    draw->AddRect(origin, ImVec2{origin.x + grid_width, origin.y + grid_height},
                  ImGui::GetColorU32(theme.border));

    const ImVec2 mouse = ImGui::GetMousePos();
    const ImVec2 local{mouse.x - origin.x, mouse.y - origin.y};

    // Live gesture deltas — the preview offsets below and the commit
    // values at release. Moves snap to whole rows (the grid the click
    // path already snaps to); resize snaps to rows with a 1-tick floor.
    int move_dtick = 0;
    int move_dpitch = 0;
    if (drag_ == Drag::kMove) {
        const float row_px = kTickWidth * static_cast<float>(ticks_per_row);
        move_dtick =
            static_cast<int>(std::lround((local.x - drag_press_.x) / row_px)) * ticks_per_row;
        move_dpitch = -static_cast<int>(std::lround((local.y - drag_press_.y) / kRowHeight));
    }
    int resize_duration = 0;
    if (drag_ == Drag::kResize && resize_note_ >= 0) {
        const engine::SequenceNote& orig = layer->notes[static_cast<std::size_t>(resize_note_)];
        const int dtick = static_cast<int>(std::lround((local.x - drag_press_.x) / kTickWidth));
        resize_duration =
            static_cast<int>(std::lround(static_cast<float>(orig.duration_ticks + dtick) /
                                         static_cast<float>(ticks_per_row))) *
            ticks_per_row;
        resize_duration = std::max(1, resize_duration);
    }

    // Add-mode draw gesture (row-snapped): start row from the press, end
    // row from the current mouse, duration at least one row. A stationary
    // press yields a one-row note — the classic click-to-add.
    int draw_start_tick = 0;
    int draw_duration = 0;
    int draw_pitch = 0;
    if (drag_ == Drag::kDraw) {
        const int start_raw = std::clamp(
            static_cast<int>(std::min(drag_press_.x, local.x) / kTickWidth), 0, total_ticks - 1);
        draw_start_tick = (start_raw / ticks_per_row) * ticks_per_row;
        const int end_raw =
            std::clamp(static_cast<int>(std::ceil(std::max(drag_press_.x, local.x) / kTickWidth)),
                       draw_start_tick + 1, total_ticks);
        const int span =
            ((end_raw - draw_start_tick + ticks_per_row - 1) / ticks_per_row) * ticks_per_row;
        draw_duration = std::max(ticks_per_row, std::min(span, total_ticks - draw_start_tick));
        draw_pitch = base_pitch_ +
                     (kPitchRows - 1 -
                      std::clamp(static_cast<int>(drag_press_.y / kRowHeight), 0, kPitchRows - 1));
    }

    // Notes; selected ones fill with the text color, and an active
    // gesture renders at its preview position.
    int hovered_note = -1;
    if (layer != nullptr) {
        for (std::size_t n = 0; n < layer->notes.size(); ++n) {
            const int index = static_cast<int>(n);
            engine::SequenceNote note = layer->notes[n];
            const bool selected = is_selected(index);
            if (drag_ == Drag::kMove && selected) {
                note.start_tick = std::clamp(note.start_tick + move_dtick, 0, total_ticks - 1);
                note.pitch = std::clamp(note.pitch + move_dpitch, 0, 127);
            } else if (drag_ == Drag::kResize && index == resize_note_) {
                note.duration_ticks = resize_duration;
            }
            const int row_offset = note.pitch - base_pitch_;
            if (row_offset < 0 || row_offset >= kPitchRows) {
                continue;
            }
            const float x0 = origin.x + (static_cast<float>(note.start_tick) * kTickWidth);
            const float x1 =
                origin.x + (static_cast<float>(note.start_tick + note.duration_ticks) * kTickWidth);
            const float y0 =
                origin.y + (static_cast<float>(kPitchRows - 1 - row_offset) * kRowHeight);
            draw->AddRectFilled(ImVec2{x0 + 1, y0 + 1}, ImVec2{x1 - 1, y0 + kRowHeight - 1},
                                ImGui::GetColorU32(selected ? theme.text : theme.primary));
            if (selected) {
                draw->AddRect(ImVec2{x0, y0}, ImVec2{x1, y0 + kRowHeight},
                              ImGui::GetColorU32(theme.primary));
            }
            if (drag_ == Drag::kNone && mouse.x >= x0 && mouse.x < x1 && mouse.y >= y0 &&
                mouse.y < y0 + kRowHeight) {
                hovered_note = index;
            }
        }
    }

    if (drag_ == Drag::kMarquee) {
        const ImVec2 a{origin.x + std::min(drag_press_.x, local.x),
                       origin.y + std::min(drag_press_.y, local.y)};
        const ImVec2 b{origin.x + std::max(drag_press_.x, local.x),
                       origin.y + std::max(drag_press_.y, local.y)};
        // Rubber-band fill from the highlight colour (dark themes:
        // highlight_bg == primary, matching the old glow; arctic's 0.08
        // glow would leave the marquee invisible).
        ImVec4 fill = theme.highlight_bg;
        fill.w = 0.35F;
        draw->AddRectFilled(a, b, ImGui::GetColorU32(fill));
        draw->AddRect(a, b, ImGui::GetColorU32(theme.primary));
    }

    // Add-mode preview: the note that will be committed at release.
    if (drag_ == Drag::kDraw) {
        const int row_offset = draw_pitch - base_pitch_;
        if (row_offset >= 0 && row_offset < kPitchRows) {
            const float x0 = origin.x + (static_cast<float>(draw_start_tick) * kTickWidth);
            const float x1 =
                origin.x + (static_cast<float>(draw_start_tick + draw_duration) * kTickWidth);
            const float y0 =
                origin.y + (static_cast<float>(kPitchRows - 1 - row_offset) * kRowHeight);
            ImVec4 fill = theme.primary;
            fill.w = 0.4F;
            draw->AddRectFilled(ImVec2{x0 + 1, y0 + 1}, ImVec2{x1 - 1, y0 + kRowHeight - 1},
                                ImGui::GetColorU32(fill));
            draw->AddRect(ImVec2{x0, y0}, ImVec2{x1, y0 + kRowHeight},
                          ImGui::GetColorU32(theme.primary));
        }
    }

    // Interaction. Press decides the gesture — note body starts a
    // selection move, note tail a resize, empty space a marquee — and
    // release resolves it. Shift+click toggles membership without
    // dragging; a stationary marquee clears the selection, or with
    // nothing selected adds a one-row note (the classic click).
    ImGui::InvisibleButton("gridhit", ImVec2{grid_width, grid_height});
    const bool grid_hovered = ImGui::IsItemHovered();

    if (ImGui::IsItemActivated()) {
        drag_press_ = local;
        resize_note_ = -1;
        if (hovered_note >= 0 && layer != nullptr) {
            if (io.KeyShift) {
                toggle_selected(hovered_note);
                drag_ = Drag::kNone;
            } else {
                if (!is_selected(hovered_note)) {
                    selection_ = {hovered_note};
                }
                const engine::SequenceNote& note =
                    layer->notes[static_cast<std::size_t>(hovered_note)];
                const float note_end =
                    static_cast<float>(note.start_tick + note.duration_ticks) * kTickWidth;
                const float grip = std::min(kResizeGripPx, static_cast<float>(note.duration_ticks) *
                                                               kTickWidth * 0.5F);
                if (local.x >= note_end - grip) {
                    drag_ = Drag::kResize;
                    resize_note_ = hovered_note;
                } else {
                    drag_ = Drag::kMove;
                }
            }
        } else {
            // Empty space: add mode draws a note, select mode marquees.
            drag_ = mode_ == Mode::kAdd ? Drag::kDraw : Drag::kMarquee;
        }
    }

    if (ImGui::IsItemDeactivated() && drag_ != Drag::kNone) {
        if (drag_ == Drag::kDraw) {
            // Add mode commit (click or drag-with-duration). Adding grows
            // the layer implicitly; audition previews when the toggle is on.
            session.seq_add_note(pattern_index, channel_, layer_,
                                 {.pitch = draw_pitch,
                                  .start_tick = draw_start_tick,
                                  .duration_ticks = draw_duration,
                                  .velocity = 100});
            if (settings.piano_roll_audition) {
                audition_pitch(session, session.seq_layer(pattern_index, channel_, layer_, false),
                               draw_pitch);
            }
        } else if (drag_ == Drag::kMarquee) {
            const bool click = std::abs(local.x - drag_press_.x) < kClickSlopPx &&
                               std::abs(local.y - drag_press_.y) < kClickSlopPx;
            if (click) {
                // Select mode: an empty click just clears the selection
                // (adding notes lives in add mode).
                if (!io.KeyShift) {
                    selection_.clear();
                }
            } else if (layer != nullptr) {
                const float min_x = std::min(drag_press_.x, local.x);
                const float max_x = std::max(drag_press_.x, local.x);
                const float min_y = std::min(drag_press_.y, local.y);
                const float max_y = std::max(drag_press_.y, local.y);
                std::vector<int> picked = io.KeyShift ? selection_ : std::vector<int>{};
                for (std::size_t n = 0; n < layer->notes.size(); ++n) {
                    const engine::SequenceNote& note = layer->notes[n];
                    const int row_offset = note.pitch - base_pitch_;
                    if (row_offset < 0 || row_offset >= kPitchRows) {
                        continue;
                    }
                    const float x0 = static_cast<float>(note.start_tick) * kTickWidth;
                    const float x1 =
                        static_cast<float>(note.start_tick + note.duration_ticks) * kTickWidth;
                    const float y0 = static_cast<float>(kPitchRows - 1 - row_offset) * kRowHeight;
                    if (x1 > min_x && x0 < max_x && y0 + kRowHeight > min_y && y0 < max_y) {
                        picked.push_back(static_cast<int>(n));
                    }
                }
                std::sort(picked.begin(), picked.end());
                picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
                selection_ = std::move(picked);
            }
        } else if (drag_ == Drag::kMove && layer != nullptr &&
                   (move_dtick != 0 || move_dpitch != 0)) {
            std::vector<TaggedNote> tagged;
            tagged.reserve(layer->notes.size());
            for (std::size_t n = 0; n < layer->notes.size(); ++n) {
                TaggedNote entry{.note = layer->notes[n],
                                 .selected = is_selected(static_cast<int>(n))};
                if (entry.selected) {
                    entry.note.start_tick =
                        std::clamp(entry.note.start_tick + move_dtick, 0, total_ticks - 1);
                    entry.note.pitch = std::clamp(entry.note.pitch + move_dpitch, 0, 127);
                }
                tagged.push_back(entry);
            }
            replace_notes(session, pattern_index, std::move(tagged));
        } else if (drag_ == Drag::kResize && layer != nullptr && resize_note_ >= 0 &&
                   resize_duration !=
                       layer->notes[static_cast<std::size_t>(resize_note_)].duration_ticks) {
            std::vector<TaggedNote> tagged;
            tagged.reserve(layer->notes.size());
            for (std::size_t n = 0; n < layer->notes.size(); ++n) {
                TaggedNote entry{.note = layer->notes[n],
                                 .selected = is_selected(static_cast<int>(n))};
                if (static_cast<int>(n) == resize_note_) {
                    entry.note.duration_ticks = resize_duration;
                }
                tagged.push_back(entry);
            }
            replace_notes(session, pattern_index, std::move(tagged));
        }
        drag_ = Drag::kNone;
        resize_note_ = -1;
    }

    if (grid_hovered && drag_ == Drag::kNone) {
        if (hovered_note >= 0 && layer != nullptr) {
            const engine::SequenceNote& note = layer->notes[static_cast<std::size_t>(hovered_note)];
            const float note_end =
                static_cast<float>(note.start_tick + note.duration_ticks) * kTickWidth;
            const float grip = std::min(kResizeGripPx, static_cast<float>(note.duration_ticks) *
                                                           kTickWidth * 0.5F);
            if (local.x >= note_end - grip) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_note >= 0) {
            session.seq_remove_note(pattern_index, channel_, layer_, hovered_note);
            // Reindex the selection across the removal.
            std::erase(selection_, hovered_note);
            for (int& index : selection_) {
                if (index > hovered_note) {
                    --index;
                }
            }
        }
        const int tick = std::clamp(static_cast<int>(local.x / kTickWidth), 0, total_ticks - 1);
        const int row_tick = (tick / ticks_per_row) * ticks_per_row;
        const int pitch =
            base_pitch_ + (kPitchRows - 1 -
                           std::clamp(static_cast<int>(local.y / kRowHeight), 0, kPitchRows - 1));
        ImGui::SetTooltip("%s%d tick %d", note_name(pitch), (pitch / 12) - 1, row_tick);
    }
    ImGui::EndChild();

    // Selection keys. Escape cancels a live gesture, else clears the
    // selection; Ctrl+C copies it rebased to its earliest tick (the
    // notes vector is tick-sorted, so the first selected index holds
    // it); Ctrl+V pastes at the first row boundary at or right of the
    // grid's horizontal scroll — the web editor's snapTick(scrollX)
    // anchor — and the clones become the selection.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (drag_ != Drag::kNone) {
                drag_ = Drag::kNone;
                resize_note_ = -1;
            } else {
                selection_.clear();
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && layer != nullptr &&
            !selection_.empty()) {
            const int base = layer->notes[static_cast<std::size_t>(selection_.front())].start_tick;
            clipboard_.clear();
            clipboard_.reserve(selection_.size());
            for (const int index : selection_) {
                engine::SequenceNote note = layer->notes[static_cast<std::size_t>(index)];
                note.start_tick -= base;
                clipboard_.push_back(note);
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && !clipboard_.empty()) {
            int span = 1;
            for (const engine::SequenceNote& note : clipboard_) {
                span = std::max(span, note.start_tick + note.duration_ticks);
            }
            const int first_visible = static_cast<int>(std::ceil(scroll_x / kTickWidth));
            int anchor = ((first_visible + ticks_per_row - 1) / ticks_per_row) * ticks_per_row;
            anchor = std::clamp(anchor, 0, std::max(0, total_ticks - span));
            std::vector<TaggedNote> tagged;
            tagged.reserve((layer != nullptr ? layer->notes.size() : 0) + clipboard_.size());
            if (layer != nullptr) {
                for (const engine::SequenceNote& note : layer->notes) {
                    tagged.push_back({.note = note, .selected = false});
                }
            }
            for (const engine::SequenceNote& note : clipboard_) {
                TaggedNote entry{.note = note, .selected = true};
                entry.note.start_tick = std::min(entry.note.start_tick + anchor, total_ticks - 1);
                entry.note.duration_ticks =
                    std::clamp(entry.note.duration_ticks, 1, total_ticks - entry.note.start_tick);
                tagged.push_back(entry);
            }
            replace_notes(session, pattern_index, std::move(tagged));
        }
    }

    // ── On-screen keyboard (two octaves, audition path) ──────────────
    const int kb_base = base_pitch_ + 12;
    ImGui::TextDisabled("keyboard");
    for (int k = 0; k < 24; ++k) {
        const int pitch = kb_base + k;
        if (k > 0) {
            ImGui::SameLine(0.0F, 2.0F);
        }
        std::array<char, 24> label{};
        std::snprintf(label.data(), label.size(), "%s%d##kb%d", note_name(pitch), (pitch / 12) - 1,
                      k);
        const bool black = is_black_key(pitch);
        if (black) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.1F, 0.08F, 0.05F, 1.0F});
        }
        ImGui::Button(label.data(), ImVec2{26.0F, black ? 30.0F : 42.0F});
        if (black) {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemActivated()) {
            const int slot = layer != nullptr && layer->instrument > 0 ? layer->instrument : 1;
            // Sample instruments audition through the channel preview;
            // plugin instruments through the note command path.
            const auto& table = project.instrument_table;
            const bool is_plugin = slot >= 1 && slot <= static_cast<int>(table.size()) &&
                                   table[static_cast<std::size_t>(slot - 1)].type !=
                                       engine::InstrumentSourceType::kSample;
            if (is_plugin) {
                session.preview_plugin_note(slot, pitch, 0.9F);
            } else {
                session.preview_note(channel_, slot, pitch - 11); // MIDI → tracker note
            }
            keyboard_note_down_ = pitch;
        }
        if (ImGui::IsItemDeactivated() && keyboard_note_down_ == pitch) {
            session.preview_plugin_release();
            keyboard_note_down_ = -1;
        }
    }

    ImGui::End();
}

} // namespace nt::ui
