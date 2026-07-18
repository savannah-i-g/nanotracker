#include "ui/theme.h"

#include <array>

namespace nt::ui {

namespace {

constexpr float kGlowAlpha = 0.35F; // primary → soft glow/hover fills

ImVec4 rgb(unsigned hex) {
    return {static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(hex & 0xFFU) / 255.0F, 1.0F};
}

ImVec4 scale_rgb(const ImVec4& c, float factor, float alpha = 1.0F) {
    return {c.x * factor, c.y * factor, c.z * factor, alpha};
}

// Explicit palettes from the web renderer's THEMES table
// (trackerRenderer.ts:80-85): primary, bg, primaryDim, bgElevated,
// text, textDim, border. primaryGlow is primary at 0.15 alpha.
struct ThemeSpec {
    const char* id;
    const char* name;
    unsigned primary, bg, primary_dim, bg_elevated, text, text_dim, border;
};

Theme make_theme(const ThemeSpec& s) {
    Theme t{};
    t.id = s.id;
    t.name = s.name;
    t.primary = rgb(s.primary);
    t.background = rgb(s.bg);
    t.primary_dim = rgb(s.primary_dim);
    t.primary_glow = scale_rgb(t.primary, 1.0F, kGlowAlpha);
    t.background_alt = rgb(s.bg_elevated);
    t.text = rgb(s.text);
    t.text_dim = rgb(s.text_dim);
    t.border = rgb(s.border);
    return t;
}

const std::array<Theme, 4> kThemes = {
    make_theme(
        {"amber", "AMBER", 0xFFAA00, 0x1A1000, 0x885500, 0x221800, 0xCC8800, 0x775500, 0x664400}),
    make_theme(
        {"green", "GREEN", 0x33FF00, 0x001A00, 0x1A8800, 0x002200, 0x22BB00, 0x117700, 0x116600}),
    make_theme(
        {"blue", "BLUE", 0x00AAFF, 0x000D1A, 0x005588, 0x001222, 0x0088CC, 0x005577, 0x004466}),
    make_theme(
        {"red", "RED", 0xFF0A0A, 0x120000, 0x990000, 0x1C0000, 0xCC0808, 0x6E0000, 0x550000}),
};

// FX_CHANNEL_COLORS from the web trackerFxEngine.ts — 32 phosphor hues.
constexpr std::array<unsigned, 32> kChannelColors = {
    0xFF6600, 0x00BBFF, 0xFF00AA, 0x00FF88, 0xFFAA00, 0xAA00FF, 0xFF2222, 0x22FFFF,
    0x88FF00, 0xFF8800, 0x0088FF, 0xFF0055, 0x00FFCC, 0xCC00FF, 0xFFCC00, 0x00CCFF,
    0xFF3366, 0x33FF66, 0x6633FF, 0xFF9933, 0x33FFCC, 0xCC33FF, 0xFFCC33, 0x33CCFF,
    0xFF6633, 0x33FF99, 0x9933FF, 0xFF9966, 0x66FF33, 0xFF33CC, 0x33CCFF, 0xCCFF33,
};

} // namespace

std::span<const Theme> themes() {
    return kThemes;
}

ImU32 channel_color(int channel_index) {
    const unsigned hex =
        kChannelColors[static_cast<std::size_t>(channel_index) % kChannelColors.size()];
    return IM_COL32((hex >> 16U) & 0xFFU, (hex >> 8U) & 0xFFU, hex & 0xFFU, 0xFF);
}

const Theme& theme_by_id(std::string_view id) {
    for (const Theme& t : kThemes) {
        if (id == t.id) {
            return t;
        }
    }
    return kThemes.front();
}

void apply_theme(const Theme& theme) {
    ImGuiStyle& style = ImGui::GetStyle();

    // Sharp-cornered, thin-bordered chrome: the web app's windows are
    // rectangles with 1px phosphor borders, no rounding anywhere.
    style.WindowRounding = 0.0F;
    style.FrameRounding = 0.0F;
    style.ScrollbarRounding = 0.0F;
    style.GrabRounding = 0.0F;
    style.TabRounding = 0.0F;
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;

    auto& c = style.Colors;
    const ImVec4& p = theme.primary;
    const ImVec4& dim = theme.primary_dim;
    const ImVec4& glow = theme.primary_glow;
    const ImVec4& bg = theme.background;
    const ImVec4& alt = theme.background_alt;

    c[ImGuiCol_Text] = p;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = bg;
    c[ImGuiCol_Border] = dim;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = alt;
    c[ImGuiCol_FrameBgHovered] = glow;
    c[ImGuiCol_FrameBgActive] = scale_rgb(p, 1.0F, 0.5F);
    c[ImGuiCol_TitleBg] = alt;
    c[ImGuiCol_TitleBgActive] = scale_rgb(p, 0.25F);
    c[ImGuiCol_TitleBgCollapsed] = alt;
    c[ImGuiCol_MenuBarBg] = alt;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_ScrollbarGrab] = dim;
    c[ImGuiCol_ScrollbarGrabHovered] = p;
    c[ImGuiCol_ScrollbarGrabActive] = p;
    c[ImGuiCol_CheckMark] = p;
    c[ImGuiCol_SliderGrab] = p;
    c[ImGuiCol_SliderGrabActive] = p;
    c[ImGuiCol_Button] = alt;
    c[ImGuiCol_ButtonHovered] = glow;
    c[ImGuiCol_ButtonActive] = scale_rgb(p, 1.0F, 0.5F);
    c[ImGuiCol_Header] = glow;
    c[ImGuiCol_HeaderHovered] = scale_rgb(p, 1.0F, 0.45F);
    c[ImGuiCol_HeaderActive] = scale_rgb(p, 1.0F, 0.55F);
    c[ImGuiCol_Separator] = dim;
    c[ImGuiCol_SeparatorHovered] = p;
    c[ImGuiCol_SeparatorActive] = p;
    c[ImGuiCol_ResizeGrip] = dim;
    c[ImGuiCol_ResizeGripHovered] = p;
    c[ImGuiCol_ResizeGripActive] = p;
    c[ImGuiCol_Tab] = alt;
    c[ImGuiCol_TabHovered] = glow;
    c[ImGuiCol_TabSelected] = scale_rgb(p, 0.30F);
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = alt;
    c[ImGuiCol_DockingPreview] = glow;
    c[ImGuiCol_DockingEmptyBg] = bg;
    c[ImGuiCol_PlotLines] = p;
    c[ImGuiCol_PlotHistogram] = p;
    c[ImGuiCol_TableBorderStrong] = dim;
    c[ImGuiCol_TableBorderLight] = scale_rgb(dim, 0.6F);
    c[ImGuiCol_TableRowBg] = bg;
    c[ImGuiCol_TableRowBgAlt] = alt;
    c[ImGuiCol_TextSelectedBg] = glow;
    c[ImGuiCol_DragDropTarget] = p;
    c[ImGuiCol_NavCursor] = p;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6F);
}

} // namespace nt::ui
