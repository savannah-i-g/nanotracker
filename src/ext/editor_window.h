// ext/editor_window — plugin editor GUIs as separate OS windows, never
// embedded in ImGui (08-external-plugins.md). The host opens one
// top-level window per editor (ext/editor_host_surface.h — X11 or
// Win32) and the plugin embeds its own window into it via the CLAP gui
// extension.
//
// The window must be pumped once per UI frame: it drains OS events
// (close requests), and the plugin pump polls any file descriptors
// registered through clap.posix-fd-support (Linux) and fires due
// clap.timer-support timers — without those host extensions plugin
// GUIs starve.
#pragma once

#include "ext/clap_host.h"
#include "ext/editor_host_surface.h"

#include <memory>
#include <string>

namespace nt::ext {

class ClapEditorWindow {
public:
    // Opens the plugin's editor for this platform's window API. Null
    // with `error` set when the plugin has no gui extension, does not
    // support the platform API, or the display is unavailable
    // (headless run) — the auto-param panel remains.
    static std::unique_ptr<ClapEditorWindow> open(ClapPlugin& plugin, std::string& error);
    ~ClapEditorWindow();

    ClapEditorWindow(const ClapEditorWindow&) = delete;
    ClapEditorWindow& operator=(const ClapEditorWindow&) = delete;
    ClapEditorWindow(ClapEditorWindow&&) = delete;
    ClapEditorWindow& operator=(ClapEditorWindow&&) = delete;

    // Once per UI frame. False = the user closed the window (caller
    // destroys this object; the plugin gui is torn down first).
    bool update();

private:
    ClapEditorWindow() = default;

    ClapPlugin* plugin_ = nullptr;
    std::unique_ptr<EditorHostSurface> surface_;
    bool gui_created_ = false;
};

} // namespace nt::ext
