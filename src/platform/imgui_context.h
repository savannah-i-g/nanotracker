// platform/imgui_context — Dear ImGui lifetime and per-frame plumbing.
// Owns backend init/teardown, docking configuration, and the Kode Mono
// font atlas including DPI-aware rebuilds. Layout persistence is
// deliberately disabled here (io.IniFilename = nullptr): window
// placement belongs to the application's own state, never imgui.ini.
#pragma once

#include <filesystem>

namespace nt::platform {

class AppWindow;

class ImGuiHost {
public:
    // `ui_font` is the regular-weight TTF used at kFontSizePx (scaled
    // by monitor content scale). Throws std::runtime_error on backend
    // or font failure.
    ImGuiHost(AppWindow& window, std::filesystem::path ui_font);
    ~ImGuiHost();

    ImGuiHost(const ImGuiHost&) = delete;
    ImGuiHost& operator=(const ImGuiHost&) = delete;
    ImGuiHost(ImGuiHost&&) = delete;
    ImGuiHost& operator=(ImGuiHost&&) = delete;

    // Starts an ImGui frame. Detects content-scale changes and rebuilds
    // the font atlas before the frame begins (never mid-frame).
    void begin_frame();

    // Finalises the frame and issues the backend draw calls into
    // whatever framebuffer is currently bound.
    static void end_frame();

    // Scripted-input runs inject positions via io.AddMousePosEvent, but
    // the GLFW backend polls the hardware cursor every frame while the
    // window is focused and the OS cursor is outside it
    // (imgui_impl_glfw UpdateMouseData fallback), overwriting the
    // injected positions. Marking the cursor "inside" through the
    // backend's own enter callback disables that fallback for the run.
    void enable_scripted_mouse();

    static constexpr float kFontSizePx = 16.0F;

private:
    void build_fonts(float scale);

    AppWindow& window_;
    std::filesystem::path ui_font_;
    float font_scale_ = 1.0F;
};

} // namespace nt::platform
