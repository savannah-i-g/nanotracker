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

// Cross-platform latch the event pump writes: Win32 fills it from the
// window procedure, X11 from ConfigureNotify/ClientMessage handling.
struct EditorHostSurfaceState {
    bool closed = false;
    bool resized = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class EditorHostSurface {
public:
    // Opens a top-level window. `resizable` decides whether the user
    // may resize it (plugins negotiate this: CLAP can_resize / VST3
    // canResize); non-resizable windows get fixed WM constraints.
    // Null with `error` set when no display is available (headless
    // run) — callers fall back to the auto-param panel.
    static std::unique_ptr<EditorHostSurface> open(const std::string& title, std::uint32_t width,
                                                   std::uint32_t height, bool resizable,
                                                   std::string& error);
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

    // Consumes a user-driven resize (client area) recorded since the
    // last call. Programmatic set_size echoes are filtered out, so a
    // true return is always the user's hand — callers renegotiate the
    // size with the plugin and answer with set_size.
    bool take_resize(std::uint32_t& width, std::uint32_t& height) {
        if (!state_.resized) {
            return false;
        }
        state_.resized = false;
        if (state_.width == expected_width_ && state_.height == expected_height_) {
            return false; // echo of our own set_size
        }
        expected_width_ = state_.width;
        expected_height_ = state_.height;
        width = state_.width;
        height = state_.height;
        return true;
    }

    // Native handle for embedding: X11 Window on Linux, HWND on
    // Windows.
    [[nodiscard]] std::uintptr_t native_handle() const { return window_; }

private:
    EditorHostSurface() = default;

    [[maybe_unused]] void* display_ = nullptr;      // X11 Display*; unused on Win32
    std::uintptr_t window_ = 0;                     // X11 Window / HWND
    [[maybe_unused]] std::uintptr_t wm_delete_ = 0; // X11 WM_DELETE_WINDOW atom; unused on Win32
    EditorHostSurfaceState state_;
    bool resizable_ = false;
    std::uint32_t expected_width_ = 0; // last size we set programmatically
    std::uint32_t expected_height_ = 0;
};

} // namespace nt::ext
