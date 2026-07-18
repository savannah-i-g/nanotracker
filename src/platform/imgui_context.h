// platform/imgui_context — Dear ImGui lifetime and per-frame plumbing.
// Owns backend init/teardown, docking configuration, layout persistence
// (io.IniFilename → config_dir()/layout.ini) and the Kode Mono font.
// The font is loaded dynamically sized (ImGui 1.92 bakes glyphs per
// requested size), so scale changes never rebuild the atlas.
#pragma once

#include <filesystem>
#include <string>

namespace nt::platform {

class AppWindow;

class ImGuiHost {
public:
    // `ui_font` is the regular-weight TTF, loaded dynamically sized; the
    // live size is kFontSizePx driven through the style's font-scale
    // fields (apply_font_scale). Throws std::runtime_error on backend or
    // font failure.
    ImGuiHost(AppWindow& window, std::filesystem::path ui_font);
    ~ImGuiHost();

    ImGuiHost(const ImGuiHost&) = delete;
    ImGuiHost& operator=(const ImGuiHost&) = delete;
    ImGuiHost(ImGuiHost&&) = delete;
    ImGuiHost& operator=(ImGuiHost&&) = delete;

    // Starts an ImGui frame. Static like end_frame: both operate on the
    // single global ImGui context, not per-instance state.
    static void begin_frame();

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

    // The 1.92 dynamic-font path: sets the style's font-scale fields (no
    // atlas rebuild). Must run after apply_style_metrics (which resets
    // them) and only outside NewFrame..Render.
    static void apply_font_scale(float content_scale, float user_scale);

    // Base UI font size in points; the owner asked the default down
    // (~15%) from the original 16 px. The live size is this times the
    // style's font-scale fields.
    static constexpr float kFontSizePx = 14.0F;

private:
    AppWindow& window_;
    std::filesystem::path ui_font_;
    // io.IniFilename aliases this string for the context's lifetime.
    std::string layout_ini_path_;
    bool layout_file_existed_ = false;
};

} // namespace nt::platform
