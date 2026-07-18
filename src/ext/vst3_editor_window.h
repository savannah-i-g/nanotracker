// ext/vst3_editor_window — VST3 plugin editors as separate OS windows,
// mirroring ext/editor_window (CLAP): one EditorHostSurface per editor
// (X11 or Win32 — 08-external-plugins.md), the plugin's IPlugView
// attached to its native handle.
//
// The window must be updated once per UI frame: it drains OS events
// (close requests) and honours plugin resize requests. The Linux
// fd/timer plumbing VST3 editors starve without lives in
// ext/vst3_run_loop (exposed through the IPlugFrame set here AND the
// factory host context — Research/07); the session dispatches it every
// frame alongside these updates. Win32 views ride the surface's
// message pump and need no run loop.
#pragma once

#include "ext/editor_host_surface.h"
#include "ext/vst3_host.h"

#include <memory>
#include <string>

namespace nt::ext {

class Vst3EditorWindow {
public:
    // Opens the plugin's editor for this platform's window API. Null
    // with `error` set when the plugin has no editor view, the view
    // rejects the platform type, or the display is unavailable
    // (headless run) — the auto-param panel remains.
    static std::unique_ptr<Vst3EditorWindow> open(Vst3Plugin& plugin, std::string& error);
    ~Vst3EditorWindow();

    Vst3EditorWindow(const Vst3EditorWindow&) = delete;
    Vst3EditorWindow& operator=(const Vst3EditorWindow&) = delete;
    Vst3EditorWindow(Vst3EditorWindow&&) = delete;
    Vst3EditorWindow& operator=(Vst3EditorWindow&&) = delete;

    // Once per UI frame. False = the user closed the window (caller
    // destroys this object; the view is removed() before the surface
    // dies).
    bool update();

private:
    Vst3EditorWindow() = default;

    struct Impl; // hides SDK types from this header
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<EditorHostSurface> surface_;
};

} // namespace nt::ext
