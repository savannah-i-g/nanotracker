// platform/app_window — the OS window and OpenGL context.
// The only module that talks to the windowing system (GLFW). Everything
// above works with this class; no GLFW types leak past this header.
#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace nt::platform {

// Owns one OS window with an OpenGL 3.3 core context and vsync enabled.
// Lifetime: construct → loop { begin_frame(); …; end_frame(); } while
// !should_close() → destruct. Construction throws std::runtime_error if
// the window or context cannot be created.
class AppWindow {
public:
    struct Config {
        std::string title;
        int width = 1280;
        int height = 720;
    };

    explicit AppWindow(const Config& config);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;
    AppWindow(AppWindow&&) = delete;
    AppWindow& operator=(AppWindow&&) = delete;

    [[nodiscard]] bool should_close() const;

    // Polls OS events and clears the framebuffer to the given color.
    void begin_frame(float r, float g, float b);

    // Presents the frame (buffer swap; blocks on vsync).
    void end_frame();

    // Framebuffer size in pixels (may differ from window size under DPI
    // scaling; consumers must use this for GL viewport math).
    void framebuffer_size(int& width, int& height) const;

    // Window size in screen coordinates (what settings persist).
    void window_size(int& width, int& height) const;

    // Monitor content scale, for font rasterisation decisions.
    [[nodiscard]] float content_scale() const;

    [[nodiscard]] GLFWwindow* native_handle() const { return window_; }

private:
    GLFWwindow* window_ = nullptr;
};

} // namespace nt::platform
