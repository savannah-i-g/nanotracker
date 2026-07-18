// platform/imgui_context — Dear ImGui lifetime and per-frame plumbing.
// Owns backend init/teardown, docking configuration, layout persistence
// (io.IniFilename → config_dir()/layout.ini) and the Kode Mono font
// atlas including DPI-aware rebuilds.
#pragma once

#include <filesystem>
#include <string>

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

    // False when no layout.ini existed at construction — the first run
    // on this config. The application builds its default dock layout in
    // that case; otherwise ImGui restores the saved one.
    [[nodiscard]] bool layout_file_existed() const { return layout_file_existed_; }

    static constexpr float kFontSizePx = 16.0F;

private:
    void build_fonts(float scale);

    AppWindow& window_;
    std::filesystem::path ui_font_;
    // io.IniFilename aliases this string for the context's lifetime.
    std::string layout_ini_path_;
    bool layout_file_existed_ = false;
    float font_scale_ = 1.0F;
};

} // namespace nt::platform
