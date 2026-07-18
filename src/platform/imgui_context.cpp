#include "platform/imgui_context.h"

#include "platform/app_window.h"

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
    io.IniFilename = nullptr; // layout is application state, not imgui.ini

    if (!ImGui_ImplGlfw_InitForOpenGL(window_.native_handle(), /*install_callbacks=*/true)) {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui GLFW backend init failed");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui OpenGL3 backend init failed");
    }

    font_scale_ = window_.content_scale();
    build_fonts(font_scale_);
}

ImGuiHost::~ImGuiHost() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiHost::build_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFont* font = io.Fonts->AddFontFromFileTTF(ui_font_.string().c_str(), kFontSizePx * scale);
    if (font == nullptr) {
        throw std::runtime_error("failed to load UI font: " + ui_font_.string());
    }
}

void ImGuiHost::begin_frame() {
    // Content scale changes when the window moves between monitors with
    // different DPI; the atlas must be rebuilt outside a frame.
    const float scale = window_.content_scale();
    if (scale != font_scale_) {
        font_scale_ = scale;
        build_fonts(scale);
        ImGui_ImplOpenGL3_DestroyDeviceObjects();
        ImGui_ImplOpenGL3_CreateDeviceObjects();
    }

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
