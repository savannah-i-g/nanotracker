// ui/workspace_view — the patchbay: one floating window per workspace
// node with jack rails (inputs left, outputs right), the per-frame jack
// anchor registry feeding the cable overlay, and workspace-level
// controls (add sum node, cable physics settings, connection status).
//
// Windowing follows the hybrid decision: workspace windows never dock
// (jacks are meaningless in tabs). Built-in nodes (tracker bus, master
// in, module player) are pinned — no close button; utility sum nodes
// close via titlebar. Minimised (collapsed) windows keep a compact jack
// strip along the titlebar so cables stay anchored — the behaviour the
// web faked with phantom DOM elements survives, the machinery does not
// (fix #12).
#pragma once

#include "app/project_session.h"
#include "io/settings.h"
#include "ui/cable_overlay.h"
#include "ui/ntp_ui.h"
#include "ui/theme.h"

#include <string>
#include <vector>

namespace nt::ui {

class WorkspaceView {
public:
    // Draws every node window, then the cable overlay above them.
    void draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme);

private:
    // Returns true when the user closed the window (utility/plugin
    // nodes); the caller dispatches removal after the draw loop.
    bool draw_node_window(app::ProjectSession& session, graph::Node& node, const Theme& theme,
                          int layout_generation);
    void draw_jack(const graph::Node& node, const graph::Port& port, bool is_input,
                   const Theme& theme);
    void draw_collapsed_jack_strip(const graph::Node& node);
    void draw_control_window(app::ProjectSession& session, io::Settings& settings);

    CableOverlay overlay_;
    NtpUi plugin_ui_;
    std::vector<JackAnchor> anchors_;
    std::string status_;
    float status_ttl_ = 0.0F;
    int seen_layout_generation_ = -1;
};

} // namespace nt::ui
