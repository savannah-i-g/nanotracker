// ui/cable_overlay — patch-cable rendering and interaction on top of
// the workspace windows. Direct port of the web's verlet rope simulator
// (Source/.../src/lib/cablePhysics.ts) drawing into ImGui's foreground
// draw list; jack anchors come from the workspace view's per-frame
// registry, so cables track window movement with zero lag (fix #12 —
// the web queried the DOM per frame and kept phantom windows alive just
// to anchor cables).
//
// Interaction parity with CableOverlay.tsx: drag from an output jack,
// preview follows the cursor, compatible inputs highlight; right-click
// a cable to delete it; the midpoint chip toggles tap/reroute. Kind
// drives colour/dash exactly as the web (audio solid, sidechain long
// dash, cv green, gate short dash, midi amber).
#pragma once

#include "app/project_session.h"
#include "graph/graph_model.h"
#include "io/settings.h"

#include <imgui.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace nt::ui {

// Screen-space position of one jack, rebuilt every frame by the
// workspace view from live ImGui layout.
struct JackAnchor {
    std::string node_id;
    std::string port_id;
    graph::PortKind kind = graph::PortKind::kAudio;
    bool is_input = false;
    ImVec2 pos{0.0F, 0.0F};
};

// One cable's rope state (cablePhysics.ts Rope). Parallel position /
// previous-position arrays; endpoints pinned to the jack anchors each
// frame.
struct Rope {
    int segment_count = 1;
    float segment_length = 0.0F;
    std::vector<float> x, y, px, py;
};

Rope create_rope(int resolution, ImVec2 a, ImVec2 b, float slack);
void step_rope(Rope& rope, float dt, ImVec2 a, ImVec2 b, const io::Settings& settings);

class CableOverlay {
public:
    // Drag lifecycle, driven by the workspace view's jack widgets.
    void begin_drag(const JackAnchor& source);

    [[nodiscard]] bool dragging() const { return dragging_; }

    [[nodiscard]] const JackAnchor& drag_source() const { return drag_source_; }

    // Draws every cable (physics step + spline + mode chip), the drag
    // preview, and handles chip clicks / right-click delete / drag
    // drop. Failed connections append to `status` (fix #13).
    void draw(app::ProjectSession& session, const std::vector<JackAnchor>& anchors,
              const io::Settings& settings, std::string& status);

private:
    void draw_one(ImDrawList* draw, const Rope& rope, ImU32 colour, float thickness, float dash_on,
                  float dash_off);

    std::unordered_map<std::string, Rope> ropes_;
    Rope preview_;
    bool dragging_ = false;
    bool preview_valid_ = false;
    JackAnchor drag_source_;
    std::vector<ImVec2> scratch_points_; // sampled spline, reused per cable
};

} // namespace nt::ui
