#include "ui/cable_overlay.h"

#include <algorithm>
#include <cmath>

namespace nt::ui {

namespace {

// Web cable palette (cableSettings.ts defaults + CableOverlay.tsx kind
// styling). Colour is identity, not theme — kept verbatim.
constexpr ImU32 kTapColour = IM_COL32(0xFF, 0x66, 0x00, 0xFF);     // #ff6600
constexpr ImU32 kRerouteColour = IM_COL32(0x60, 0xC0, 0xFF, 0xFF); // #60c0ff
constexpr ImU32 kPreviewColour = IM_COL32(0xFF, 0xD0, 0x60, 0xFF); // #ffd060
constexpr ImU32 kCvColour = IM_COL32(0xA0, 0xE0, 0x60, 0xFF);      // #a0e060
constexpr ImU32 kMidiColour = IM_COL32(0xE6, 0xB8, 0x4A, 0xFF);    // #e6b84a

float distance(ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt((dx * dx) + (dy * dy));
}

// Kind → colour/dash, CableOverlay.tsx:406-427. Reroute wins the
// colour except for cv (which keeps its green identity in the web's
// var() indirection only when tap — match the literal behaviour:
// reroute overrides for every kind but the dash pattern stays).
struct CableStyle {
    ImU32 colour;
    float dash_on;
    float dash_off;
};

CableStyle style_for(graph::PortKind kind, graph::CableMode mode) {
    ImU32 colour = mode == graph::CableMode::kReroute ? kRerouteColour : kTapColour;
    float dash_on = 0.0F;
    float dash_off = 0.0F;
    switch (kind) {
    case graph::PortKind::kAudio:
        break;
    case graph::PortKind::kSidechain:
        dash_on = 10.0F;
        dash_off = 5.0F;
        break;
    case graph::PortKind::kCv:
        colour = kCvColour;
        break;
    case graph::PortKind::kGate:
        dash_on = 2.0F;
        dash_off = 4.0F;
        break;
    case graph::PortKind::kMidi:
        if (mode != graph::CableMode::kReroute) {
            colour = kMidiColour;
        }
        break;
    }
    return {.colour = colour, .dash_on = dash_on, .dash_off = dash_off};
}

} // namespace

Rope create_rope(int resolution, ImVec2 a, ImVec2 b, float slack) {
    Rope rope;
    rope.segment_count = std::clamp(resolution, 1, 64);
    const int points = rope.segment_count + 1;
    const float dist = distance(a, b);
    rope.segment_length = dist * slack / static_cast<float>(rope.segment_count);
    rope.x.resize(static_cast<std::size_t>(points));
    rope.y.resize(static_cast<std::size_t>(points));
    rope.px.resize(static_cast<std::size_t>(points));
    rope.py.resize(static_cast<std::size_t>(points));
    for (int i = 0; i < points; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rope.segment_count);
        const auto at = static_cast<std::size_t>(i);
        rope.x[at] = a.x + ((b.x - a.x) * t);
        rope.y[at] = a.y + ((b.y - a.y) * t);
        rope.px[at] = rope.x[at];
        rope.py[at] = rope.y[at];
    }
    return rope;
}

void step_rope(Rope& rope, float dt, ImVec2 a, ImVec2 b, const io::Settings& settings) {
    const auto last = static_cast<std::size_t>(rope.segment_count);
    // Pin endpoints before integration so jack motion is immediate.
    rope.x[0] = a.x;
    rope.y[0] = a.y;
    rope.px[0] = a.x;
    rope.py[0] = a.y;
    rope.x[last] = b.x;
    rope.y[last] = b.y;
    rope.px[last] = b.x;
    rope.py[last] = b.y;
    if (rope.segment_count == 1) {
        return; // straight line — nothing to integrate
    }

    // Rest length follows the endpoints so a stretched drag still
    // droops naturally once released.
    rope.segment_length =
        distance(a, b) * settings.cable_slack / static_cast<float>(rope.segment_count);

    const float gravity_step = settings.cable_gravity * dt * dt;
    const float keep = 1.0F - settings.cable_damping;
    for (std::size_t i = 1; i < last; ++i) {
        const float ox = rope.x[i];
        const float oy = rope.y[i];
        rope.x[i] += (ox - rope.px[i]) * keep;
        rope.y[i] += ((oy - rope.py[i]) * keep) + gravity_step;
        rope.px[i] = ox;
        rope.py[i] = oy;
    }

    const int iterations = std::max(1, settings.cable_iterations);
    for (int pass = 0; pass < iterations; ++pass) {
        for (std::size_t i = 0; i < last; ++i) {
            const std::size_t j = i + 1;
            const float dx = rope.x[j] - rope.x[i];
            const float dy = rope.y[j] - rope.y[i];
            const float dist = std::sqrt((dx * dx) + (dy * dy));
            if (dist == 0.0F) {
                continue;
            }
            const float diff = (dist - rope.segment_length) / dist;
            const float off_x = dx * 0.5F * diff;
            const float off_y = dy * 0.5F * diff;
            const bool i_pinned = i == 0;
            const bool j_pinned = j == last;
            if (i_pinned && j_pinned) {
                continue;
            }
            if (i_pinned) {
                rope.x[j] -= off_x * 2.0F;
                rope.y[j] -= off_y * 2.0F;
            } else if (j_pinned) {
                rope.x[i] += off_x * 2.0F;
                rope.y[i] += off_y * 2.0F;
            } else {
                rope.x[i] += off_x;
                rope.y[i] += off_y;
                rope.x[j] -= off_x;
                rope.y[j] -= off_y;
            }
        }
    }
}

namespace {

// Samples the rope's Catmull-Rom spline (tension 0.5, matching the
// web's SVG path generation) into a screen-space polyline.
void sample_spline(const Rope& rope, std::vector<ImVec2>& out) {
    out.clear();
    const auto last = static_cast<std::size_t>(rope.segment_count);
    out.emplace_back(rope.x[0], rope.y[0]);
    if (rope.segment_count == 1) {
        out.emplace_back(rope.x[1], rope.y[1]);
        return;
    }
    constexpr int kSubdiv = 6;
    for (std::size_t i = 0; i < last; ++i) {
        const std::size_t i0 = i == 0 ? 0 : i - 1;
        const std::size_t i3 = std::min(last, i + 2);
        const ImVec2 p0{rope.x[i0], rope.y[i0]};
        const ImVec2 p1{rope.x[i], rope.y[i]};
        const ImVec2 p2{rope.x[i + 1], rope.y[i + 1]};
        const ImVec2 p3{rope.x[i3], rope.y[i3]};
        const ImVec2 c1{p1.x + ((p2.x - p0.x) / 6.0F), p1.y + ((p2.y - p0.y) / 6.0F)};
        const ImVec2 c2{p2.x - ((p3.x - p1.x) / 6.0F), p2.y - ((p3.y - p1.y) / 6.0F)};
        for (int s = 1; s <= kSubdiv; ++s) {
            const float t = static_cast<float>(s) / kSubdiv;
            const float u = 1.0F - t;
            const float w0 = u * u * u;
            const float w1 = 3.0F * u * u * t;
            const float w2 = 3.0F * u * t * t;
            const float w3 = t * t * t;
            out.emplace_back((w0 * p1.x) + (w1 * c1.x) + (w2 * c2.x) + (w3 * p2.x),
                             (w0 * p1.y) + (w1 * c1.y) + (w2 * c2.y) + (w3 * p2.y));
        }
    }
}

float polyline_length(const std::vector<ImVec2>& points) {
    float total = 0.0F;
    for (std::size_t i = 1; i < points.size(); ++i) {
        total += distance(points[i - 1], points[i]);
    }
    return total;
}

ImVec2 point_at_length(const std::vector<ImVec2>& points, float target) {
    float walked = 0.0F;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float seg = distance(points[i - 1], points[i]);
        if (walked + seg >= target && seg > 0.0F) {
            const float t = (target - walked) / seg;
            return {points[i - 1].x + ((points[i].x - points[i - 1].x) * t),
                    points[i - 1].y + ((points[i].y - points[i - 1].y) * t)};
        }
        walked += seg;
    }
    return points.empty() ? ImVec2{0, 0} : points.back();
}

float distance_to_polyline(const std::vector<ImVec2>& points, ImVec2 p) {
    float best = 1e9F;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const ImVec2 a = points[i - 1];
        const ImVec2 b = points[i];
        const float abx = b.x - a.x;
        const float aby = b.y - a.y;
        const float len2 = (abx * abx) + (aby * aby);
        float t = len2 > 0.0F ? (((p.x - a.x) * abx) + ((p.y - a.y) * aby)) / len2 : 0.0F;
        t = std::clamp(t, 0.0F, 1.0F);
        const ImVec2 q{a.x + (abx * t), a.y + (aby * t)};
        best = std::min(best, distance(p, q));
    }
    return best;
}

} // namespace

void CableOverlay::draw_one(ImDrawList* draw, const Rope& rope, ImU32 colour, float thickness,
                            float dash_on, float dash_off) {
    sample_spline(rope, scratch_points_);
    if (scratch_points_.size() < 2) {
        return;
    }
    if (dash_on <= 0.0F) {
        draw->AddPolyline(scratch_points_.data(), static_cast<int>(scratch_points_.size()), colour,
                          ImDrawFlags_RoundCornersAll, thickness);
        return;
    }
    // Manual dashing: walk the polyline alternating pen down/up.
    bool pen = true;
    float left = dash_on;
    ImVec2 prev = scratch_points_[0];
    ImVec2 stroke_start = prev;
    for (std::size_t i = 1; i < scratch_points_.size(); ++i) {
        const ImVec2 cur = scratch_points_[i];
        float seg = distance(prev, cur);
        while (seg > 0.0F) {
            const float take = std::min(seg, left);
            const float t = take / seg;
            const ImVec2 mid{prev.x + ((cur.x - prev.x) * t), prev.y + ((cur.y - prev.y) * t)};
            if (pen) {
                draw->AddLine(stroke_start, mid, colour, thickness);
            }
            left -= take;
            seg -= take;
            prev = mid;
            if (left <= 0.0F) {
                pen = !pen;
                left = pen ? dash_on : dash_off;
                stroke_start = prev;
            }
        }
        prev = cur;
    }
}

void CableOverlay::begin_drag(const JackAnchor& source) {
    dragging_ = true;
    preview_valid_ = false;
    drag_source_ = source;
}

void CableOverlay::draw(app::ProjectSession& session, const std::vector<JackAnchor>& anchors,
                        const io::Settings& settings, std::string& status) {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0F / 30.0F);
    const graph::WorkspaceGraph& graph = session.workspace();

    auto find_anchor = [&anchors](const std::string& node_id, const std::string& port_id,
                                  bool is_input) -> const JackAnchor* {
        for (const JackAnchor& anchor : anchors) {
            if (anchor.is_input == is_input && anchor.node_id == node_id &&
                anchor.port_id == port_id) {
                return &anchor;
            }
        }
        return nullptr;
    };

    // Retire ropes whose cables are gone.
    std::erase_if(ropes_,
                  [&graph](const auto& entry) { return graph.find_cable(entry.first) == nullptr; });

    const ImVec2 mouse = ImGui::GetMousePos();
    std::string clicked_chip;
    std::string right_clicked;

    for (const graph::Cable& cable : graph.cables()) {
        const JackAnchor* a = find_anchor(cable.source.node_id, cable.source.port_id, false);
        const JackAnchor* b = find_anchor(cable.dest.node_id, cable.dest.port_id, true);
        if (a == nullptr || b == nullptr) {
            continue; // endpoint window hidden this frame
        }
        auto it = ropes_.find(cable.id);
        if (it == ropes_.end() || it->second.segment_count != settings.cable_resolution) {
            it = ropes_
                     .insert_or_assign(cable.id, create_rope(settings.cable_resolution, a->pos,
                                                             b->pos, settings.cable_slack))
                     .first;
        }
        Rope& rope = it->second;
        step_rope(rope, dt, a->pos, b->pos, settings);

        const CableStyle style = style_for(cable.src_kind, cable.mode);
        draw_one(draw, rope, style.colour, settings.cable_thickness, style.dash_on, style.dash_off);

        // Delayed-feedback marking is subtle by design: cables whose
        // dest evaluates before their source (a cycle) still read one
        // block late; the chip ring hints at nothing extra yet.

        // Midpoint chip: click toggles tap ↔ reroute.
        const float mid_len = polyline_length(scratch_points_) * 0.5F;
        const ImVec2 mid = point_at_length(scratch_points_, mid_len);
        constexpr float kChipRadius = 8.0F;
        draw->AddCircleFilled(mid, kChipRadius, IM_COL32(20, 14, 8, 230));
        draw->AddCircle(mid, kChipRadius, style.colour, 0, 1.5F);
        const bool reroute = cable.mode == graph::CableMode::kReroute;
        const char* letter = reroute ? "R" : "T";
        const ImVec2 text_size = ImGui::CalcTextSize(letter);
        draw->AddText(ImVec2{mid.x - (text_size.x * 0.5F), mid.y - (text_size.y * 0.5F)},
                      style.colour, letter);

        if (!dragging_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            distance(mouse, mid) <= kChipRadius + 2.0F) {
            clicked_chip = cable.id;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && distance(mouse, mid) > kChipRadius &&
            distance_to_polyline(scratch_points_, mouse) <=
                std::max(6.0F, settings.cable_thickness + 3.0F)) {
            right_clicked = cable.id;
        }
    }

    if (!clicked_chip.empty()) {
        const graph::Cable* cable = graph.find_cable(clicked_chip);
        if (cable != nullptr) {
            session.set_cable_mode(clicked_chip, cable->mode == graph::CableMode::kReroute
                                                     ? graph::CableMode::kTap
                                                     : graph::CableMode::kReroute);
        }
    } else if (!right_clicked.empty()) {
        session.remove_cable(right_clicked);
    }

    // ── Drag preview + drop ─────────────────────────────────────────
    if (!dragging_) {
        return;
    }
    const JackAnchor* origin = find_anchor(drag_source_.node_id, drag_source_.port_id, false);
    const ImVec2 from = origin != nullptr ? origin->pos : drag_source_.pos;
    if (!preview_valid_) {
        preview_ = create_rope(settings.cable_resolution, from, mouse, settings.cable_slack);
        preview_valid_ = true;
    }
    step_rope(preview_, dt, from, mouse, settings);
    draw_one(draw, preview_, kPreviewColour, settings.cable_thickness, 6.0F, 4.0F);

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_ = false;
        preview_valid_ = false;
        // Drop on the nearest input jack within reach.
        constexpr float kDropRadius = 14.0F;
        const JackAnchor* best = nullptr;
        float best_dist = kDropRadius;
        for (const JackAnchor& anchor : anchors) {
            if (!anchor.is_input) {
                continue;
            }
            const float d = distance(anchor.pos, mouse);
            if (d < best_dist) {
                best = &anchor;
                best_dist = d;
            }
        }
        if (best == nullptr) {
            return; // released over nothing — silent cancel, web parity
        }
        const graph::ConnectResult result = session.add_cable(
            {.node_id = drag_source_.node_id, .port_id = drag_source_.port_id},
            {.node_id = best->node_id, .port_id = best->port_id}, graph::CableMode::kTap);
        if (result != graph::ConnectResult::kOk) {
            status = std::string("cable rejected: ") + graph::connect_result_message(result);
        } else {
            status = drag_source_.port_id + " -> " + best->port_id;
        }
    }
}

} // namespace nt::ui
