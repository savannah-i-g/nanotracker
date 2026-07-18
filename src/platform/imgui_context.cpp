#include "platform/imgui_context.h"

#include "platform/app_window.h"
#include "platform/paths.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <stdexcept>

namespace nt::platform {

ImGuiHost::ImGuiHost(AppWindow& window, std::filesystem::path ui_font)
    : window_(window), ui_font_(std::move(ui_font)) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dock/window layout persists next to settings.json. Existence is
    // recorded before ImGui's first NewFrame (which loads the file) so
    // the application can tell a first run from a restored one.
    layout_ini_path_ = (config_dir() / "layout.ini").string();
    layout_file_existed_ = std::filesystem::exists(layout_ini_path_);
    io.IniFilename = layout_ini_path_.c_str();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_.native_handle(), /*install_callbacks=*/true)) {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui GLFW backend init failed");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui OpenGL3 backend init failed");
    }

    // Dynamically sized font (0 = glyphs baked per requested size by the
    // RendererHasTextures backend); the live size is driven through the
    // style's font-scale fields (apply_font_scale), never a rebuild.
    if (io.Fonts->AddFontFromFileTTF(ui_font_.string().c_str(), 0.0F) == nullptr) {
        throw std::runtime_error("failed to load UI font: " + ui_font_.string());
    }
}

ImGuiHost::~ImGuiHost() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiHost::apply_font_scale(float content_scale, float user_scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = kFontSizePx;
    style.FontScaleDpi = content_scale;
    style.FontScaleMain = user_scale;
}

void ImGuiHost::begin_frame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiHost::end_frame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiHost::enable_scripted_mouse() {
    ImGui_ImplGlfw_CursorEnterCallback(window_.native_handle(), /*entered=*/1);
}

} // namespace nt::platform
