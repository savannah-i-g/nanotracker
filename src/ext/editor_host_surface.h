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

    // Blocks until the server has processed this surface's outstanding
    // requests (X11 XSync; a no-op elsewhere). The bridge child calls it so
    // the editor window is materialised before its id crosses to the host for
    // reparenting; the in-process path never needs it.
    void sync();

    // ── Cross-process editor embedding (S29d, §D; Linux/X11 only) ────────
    // Reparents a FOREIGN window — one owned by another process (the bridge
    // child) — into this surface as the container, and maps it to fill the
    // client area. X11 window ids are display-global, so XReparentWindow works
    // across the process boundary. Because the adopted window's owner may
    // vanish mid-edit, a process-wide X error handler is installed that
    // swallows protocol errors naming an adopted window, so a dead foreign
    // window can never abort the host. Returns false if the reparent failed
    // (foreign window already gone) or on a platform without cross-process
    // embedding (Windows editor bridging is deferred, §H.2). The in-process
    // editor path never adopts a foreign window, so it is unaffected.
    bool adopt_foreign_child(std::uintptr_t child_window);

    // Relays a container resize to the adopted foreign child so it fills the
    // client area. No-op when nothing was adopted. A racing foreign-window
    // error (child just died) is swallowed by the guarded error handler.
    void resize_foreign_child(std::uint32_t width, std::uint32_t height);

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
    // Foreign (cross-process) child reparented into this container, or 0.
    // X11 only; unregistered from the error-handler guard set on destruction.
    [[maybe_unused]] std::uintptr_t foreign_window_ = 0;
    EditorHostSurfaceState state_;
    bool resizable_ = false;
    std::uint32_t expected_width_ = 0; // last size we set programmatically
    std::uint32_t expected_height_ = 0;
};

} // namespace nt::ext
