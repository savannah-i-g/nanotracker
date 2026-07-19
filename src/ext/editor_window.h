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

    // Bridge support (S29d, §D): realize the editor's OS window on the
    // server and report its native handle + size + resizability, so the
    // out-of-process host can reparent the child-created window into its own
    // container. The sync closes the cross-connection ordering gap — the host
    // reparents on a DIFFERENT X connection than the child created on, so the
    // window must be materialised server-side before the id is handed over.
    // The in-process editor path never calls this, so its behaviour is
    // unchanged.
    void describe_for_reparent(std::uintptr_t& window, std::uint32_t& width, std::uint32_t& height,
                               bool& resizable);

private:
    ClapEditorWindow() = default;

    ClapPlugin* plugin_ = nullptr;
    std::unique_ptr<EditorHostSurface> surface_;
    bool gui_created_ = false;
    // Initial editor geometry, captured at open() for the bridge to size its
    // container to (the in-process path reads the surface directly).
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool resizable_ = false;
};

} // namespace nt::ext
