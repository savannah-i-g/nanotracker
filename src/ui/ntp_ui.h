// ui/ntp_ui — declarative NTP plugin UI rendering. The manifest's
// control tree (knob/slider/toggle/select/number/xy_pad/
// envelope_editor/meter/label/group/image/sprite) renders natively in
// ImGui inside the plugin's workspace window; plugins without a usable
// control tree (or that only declared a webview on the web side) get
// the auto-generated parameter panel, the same fallback external
// plugin hosting uses.
//
// Image/sprite assets come from the plugin archive and upload lazily
// into GL textures cached per (plugin id, asset). Sprites render their
// first frame in v1 — animation keys are a post-v1 driver feature.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <map>
#include <string>

namespace nt::ui {

class NtpUi {
public:
    // Draws the body of a plugin node window (called inside Begin/End).
    void draw(app::ProjectSession& session, const std::string& workspace_id, const Theme& theme);

private:
    struct Texture {
        unsigned int id = 0;
        int width = 0;
        int height = 0;
    };

    Texture& texture_for(const plugins::LoadedNtpPlugin& plugin, const std::string& asset);

    void draw_control(plugins::NtpInstance& instance, const ntp::Manifest& manifest,
                      const plugins::LoadedNtpPlugin& plugin, const ntp::UiControl& control,
                      const Theme& theme);
    static void draw_auto_panel(plugins::NtpInstance& instance, const ntp::Manifest& manifest);
    static void draw_param_widget(plugins::NtpInstance& instance, const ntp::ParamDef& def,
                                  bool as_knob, float width_hint);

    std::map<std::string, Texture> textures_; // "<plugin id>/<asset>"
};

} // namespace nt::ui
