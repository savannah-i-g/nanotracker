// ext/editor_host_surface — the bare top-level OS window a plugin
// editor embeds into (08-external-plugins.md): X11 on Linux (Wayland
// runs through XWayland, per the platform policy), Win32 on Windows.
// One implementation TU per platform, selected by the build; the
// CLAP — and later VST3 — glue stays platform-neutral against this
// interface.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nt::ext {

class EditorHostSurface {
public:
    // Opens a top-level window with a fixed (non-user-resizable)
    // client area. Null with `error` set when no display is available
    // (headless run) — callers fall back to the auto-param panel.
    static std::unique_ptr<EditorHostSurface> open(const std::string& title, std::uint32_t width,
                                                   std::uint32_t height, std::string& error);
    ~EditorHostSurface();

    EditorHostSurface(const EditorHostSurface&) = delete;
    EditorHostSurface& operator=(const EditorHostSurface&) = delete;
    EditorHostSurface(EditorHostSurface&&) = delete;
    EditorHostSurface& operator=(EditorHostSurface&&) = delete;

    // Resizes the client area to the plugin-reported editor size.
    void set_size(std::uint32_t width, std::uint32_t height);

    // Drains OS events once per UI frame. False = the user closed the
    // window; the caller tears the plugin gui down, then this object.
    bool pump();

    // Native handle for embedding: X11 Window on Linux, HWND on
    // Windows.
    [[nodiscard]] std::uintptr_t native_handle() const { return window_; }

private:
    EditorHostSurface() = default;

    [[maybe_unused]] void* display_ = nullptr;      // X11 Display*; unused on Win32
    std::uintptr_t window_ = 0;                     // X11 Window / HWND
    [[maybe_unused]] std::uintptr_t wm_delete_ = 0; // X11 WM_DELETE_WINDOW atom; unused on Win32
    [[maybe_unused]] bool closed_ = false;          // Win32 WM_CLOSE latch; unused on X11
};

} // namespace nt::ext
