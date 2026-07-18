#include "ext/editor_host_surface.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Wide-character resource macros (IDC_ARROW et al.) to match the
// explicit W-suffixed API calls below.
#define UNICODE
#define _UNICODE

#include <vector>
#include <windows.h>

namespace nt::ext {

namespace {

constexpr const wchar_t* kClassName = L"NanoTrackerPluginEditor";

// WM_CLOSE only latches the flag — the surface destructor owns
// DestroyWindow, so teardown order (plugin gui first) stays with the
// caller.
LRESULT CALLBACK surface_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
        auto* closed = reinterpret_cast<bool*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (closed != nullptr) {
            *closed = true;
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_class_once() {
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = surface_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        return ::RegisterClassW(&wc) != 0;
    }();
    return registered;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int len =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                          len);
    return wide;
}

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

} // namespace

std::unique_ptr<EditorHostSurface> EditorHostSurface::open(const std::string& title,
                                                           std::uint32_t width,
                                                           std::uint32_t height,
                                                           std::string& error) {
    if (!register_class_once()) {
        error = "window class registration failed";
        return nullptr;
    }
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    ::AdjustWindowRect(&rect, kStyle, FALSE);
    HWND hwnd = ::CreateWindowExW(0, kClassName, widen(title).c_str(), kStyle, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                  nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) {
        error = "CreateWindowEx failed (code " + std::to_string(::GetLastError()) + ")";
        return nullptr;
    }

    auto self = std::unique_ptr<EditorHostSurface>(new EditorHostSurface());
    self->window_ = reinterpret_cast<std::uintptr_t>(hwnd);
    // The latch address is stable (heap object) and set before the
    // first pump, which is the only place WM_CLOSE can arrive.
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&self->closed_));
    ::ShowWindow(hwnd, SW_SHOW);
    return self;
}

EditorHostSurface::~EditorHostSurface() {
    if (window_ != 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
        ::DestroyWindow(reinterpret_cast<HWND>(window_));
    }
}

void EditorHostSurface::set_size(std::uint32_t width, std::uint32_t height) {
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    ::AdjustWindowRect(&rect, kStyle, FALSE);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    ::SetWindowPos(reinterpret_cast<HWND>(window_), nullptr, 0, 0, rect.right - rect.left,
                   rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool EditorHostSurface::pump() {
    MSG msg;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    HWND hwnd = reinterpret_cast<HWND>(window_);
    while (::PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE) != 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return !closed_;
}

} // namespace nt::ext
