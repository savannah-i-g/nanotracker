#include "ext/editor_host_surface.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <atomic>
#include <mutex>
#include <unordered_set>

// Xlib is a C API built on raw arrays and unions (XEvent, screen
// macros); the pro-bounds/union warnings are the API's shape, not
// ours.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)

namespace nt::ext {

namespace {

// Fixed-size windows advertise min == max so the WM removes the
// resize handles; resizable ones only pin a sane floor.
void apply_size_hints(Display* display, Window window, std::uint32_t width, std::uint32_t height,
                      bool resizable) {
    XSizeHints hints{};
    hints.flags = PMinSize | (resizable ? 0 : PMaxSize);
    hints.min_width = resizable ? 64 : static_cast<int>(width);
    hints.min_height = resizable ? 64 : static_cast<int>(height);
    hints.max_width = static_cast<int>(width);
    hints.max_height = static_cast<int>(height);
    XSetWMNormalHints(display, window, &hints);
}

// ── Foreign-window error guard (§D.2) ────────────────────────────────
// An adopted foreign window belongs to the bridge child; if that child dies
// mid-edit the server destroys the window, and any host X request still
// naming it raises a protocol error (BadWindow/BadDrawable). Xlib's default
// handler prints and calls exit() on such an error — which would take the
// host down with the plugin, the exact failure the bridge exists to prevent.
// So a process-wide handler is installed (once, on the first adopt) that
// swallows errors naming a currently-adopted foreign window and delegates
// everything else to the previously-installed handler (the app's, e.g.
// GLFW's). It is scoped by resource id, never by error class, so it cannot
// mask unrelated bugs: only errors against a window the bridge is actively
// embedding are absorbed. Installed once and left in place — it is inert
// whenever no foreign window is adopted (the guarded set is then empty).
std::mutex& guard_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_set<unsigned long>& guarded_windows() {
    static std::unordered_set<unsigned long> set;
    return set;
}

// Set by the handler when it swallows a guarded error, so adopt can tell a
// reparent that failed on a vanished window from one that completed.
std::atomic<bool>& guard_error_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

// The Xlib handler in effect before ours; unrelated errors still reach it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — Xlib handler slot
XErrorHandler g_prev_error_handler = nullptr;

int guarded_error_handler(Display* display, XErrorEvent* event) {
    {
        const std::lock_guard<std::mutex> lock(guard_mutex());
        if (guarded_windows().contains(event->resourceid)) {
            guard_error_flag().store(true, std::memory_order_relaxed);
            return 0; // a dead/foreign editor window must never abort the host
        }
    }
    if (g_prev_error_handler != nullptr) {
        return g_prev_error_handler(display, event);
    }
    return 0; // no prior handler: still never exit on a stray error
}

void install_guard_once() {
    static const bool installed = [] {
        g_prev_error_handler = XSetErrorHandler(guarded_error_handler);
        return true;
    }();
    (void)installed;
}

void register_guarded(unsigned long window) {
    install_guard_once();
    const std::lock_guard<std::mutex> lock(guard_mutex());
    guarded_windows().insert(window);
}

void unregister_guarded(unsigned long window) {
    const std::lock_guard<std::mutex> lock(guard_mutex());
    guarded_windows().erase(window);
}

} // namespace

std::unique_ptr<EditorHostSurface> EditorHostSurface::open(const std::string& title,
                                                           std::uint32_t width,
                                                           std::uint32_t height, bool resizable,
                                                           std::string& error) {
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        error = "no X display (headless run)";
        return nullptr;
    }
    const int screen = DefaultScreen(display);
    const Window window =
        XCreateSimpleWindow(display, RootWindow(display, screen), 60, 60, width, height, 0,
                            BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, title.c_str());
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);
    apply_size_hints(display, window, width, height, resizable);
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);
    XFlush(display);

    auto self = std::unique_ptr<EditorHostSurface>(new EditorHostSurface());
    self->display_ = display;
    self->window_ = window;
    self->wm_delete_ = wm_delete;
    self->resizable_ = resizable;
    self->expected_width_ = width;
    self->expected_height_ = height;
    return self;
}

EditorHostSurface::~EditorHostSurface() {
    // Stop guarding the adopted foreign window before this container's display
    // closes; the child owns and destroys that window (on close, or by dying),
    // so we never destroy it ourselves — only our own container below.
    if (foreign_window_ != 0) {
        unregister_guarded(static_cast<Window>(foreign_window_));
    }
    auto* display = static_cast<Display*>(display_);
    if (display != nullptr) {
        if (window_ != 0) {
            XDestroyWindow(display, window_);
        }
        XCloseDisplay(display);
    }
}

void EditorHostSurface::set_size(std::uint32_t width, std::uint32_t height) {
    auto* display = static_cast<Display*>(display_);
    apply_size_hints(display, window_, width, height, resizable_);
    XResizeWindow(display, window_, width, height);
    XFlush(display);
    expected_width_ = width;
    expected_height_ = height;
}

void EditorHostSurface::sync() {
    auto* display = static_cast<Display*>(display_);
    if (display != nullptr) {
        XSync(display, False);
    }
}

bool EditorHostSurface::adopt_foreign_child(std::uintptr_t child_window) {
    auto* display = static_cast<Display*>(display_);
    if (display == nullptr || child_window == 0) {
        return false;
    }
    const auto child = static_cast<Window>(child_window);
    // Register BEFORE issuing any request naming the window, so an immediate
    // BadWindow (the child already gone) is swallowed rather than fatal.
    register_guarded(child);
    guard_error_flag().store(false, std::memory_order_relaxed);
    XReparentWindow(display, child, static_cast<Window>(window_), 0, 0);
    XMapWindow(display, child);
    XSync(display, False); // force the reparent to complete so a failure surfaces now
    if (guard_error_flag().load(std::memory_order_relaxed)) {
        unregister_guarded(child); // the foreign window was gone: adoption failed
        return false;
    }
    foreign_window_ = child_window;
    return true;
}

void EditorHostSurface::resize_foreign_child(std::uint32_t width, std::uint32_t height) {
    auto* display = static_cast<Display*>(display_);
    if (display == nullptr || foreign_window_ == 0) {
        return;
    }
    // A racing child death names a dead window here; the guarded handler
    // swallows the resulting error, so no synchronous round-trip is needed.
    XMoveResizeWindow(display, static_cast<Window>(foreign_window_), 0, 0, width, height);
    XFlush(display);
}

bool EditorHostSurface::pump() {
    auto* display = static_cast<Display*>(display_);
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == ClientMessage &&
            static_cast<unsigned long>(event.xclient.data.l[0]) == wm_delete_) {
            return false;
        }
        if (event.type == ConfigureNotify && event.xconfigure.window == window_) {
            state_.resized = true;
            state_.width = static_cast<std::uint32_t>(event.xconfigure.width);
            state_.height = static_cast<std::uint32_t>(event.xconfigure.height);
        }
    }
    return true;
}

} // namespace nt::ext

// NOLINTEND(cppcoreguidelines-pro-type-union-access)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
