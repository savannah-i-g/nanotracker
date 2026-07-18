#include "ext/editor_host_surface.h"

#include <X11/Xlib.h>

// Xlib is a C API built on raw arrays and unions (XEvent, screen
// macros); the pro-bounds/union warnings are the API's shape, not
// ours.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)

namespace nt::ext {

std::unique_ptr<EditorHostSurface> EditorHostSurface::open(const std::string& title,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
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
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);
    XFlush(display);

    auto self = std::unique_ptr<EditorHostSurface>(new EditorHostSurface());
    self->display_ = display;
    self->window_ = window;
    self->wm_delete_ = wm_delete;
    return self;
}

EditorHostSurface::~EditorHostSurface() {
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
    XResizeWindow(display, window_, width, height);
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
    }
    return true;
}

} // namespace nt::ext

// NOLINTEND(cppcoreguidelines-pro-type-union-access)
// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
