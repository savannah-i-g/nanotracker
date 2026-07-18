// ui/theme — the four phosphor palettes and their mapping onto ImGui
// style colors. Palette values mirror the web app's theme.css; the
// derived-style rules (dim/glow variants scaled from primary) mirror
// its trackerRenderer conventions.
#pragma once

#include <imgui.h>
#include <span>
#include <string_view>

namespace nt::ui {

struct Theme {
    const char* id = "";   // stable settings key ("amber", "green", ...)
    const char* name = ""; // display label
    ImVec4 primary;        // phosphor foreground
    ImVec4 background;

    // Explicit palette entries mirroring the web renderer's THEMES
    // table (trackerRenderer.ts) — the pattern grid depends on these
    // exact values, not derivations.
    ImVec4 primary_dim;
    ImVec4 primary_glow;   // low-alpha primary for row/selection glow
    ImVec4 background_alt; // "bgElevated"
    ImVec4 text;
    ImVec4 text_dim;
    ImVec4 border;
};

// The web pattern editor's per-channel hue palette
// (FX_CHANNEL_COLORS / PATTERN_CHANNEL_COLORS, 32 entries).
ImU32 channel_color(int channel_index);

// The built-in palettes, in display order. Index 0 (amber) is the
// default.
std::span<const Theme> themes();

// Theme lookup by settings id; falls back to the default theme when the
// id is unknown (settings tolerance, never fatal).
const Theme& theme_by_id(std::string_view id);

// Applies the palette to the global ImGui style. Call at startup and on
// theme switch; safe between frames.
void apply_theme(const Theme& theme);

} // namespace nt::ui
