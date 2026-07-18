// ui/ntp_ui — declarative NTP plugin UI rendering. The manifest's
// control tree (knob/slider/toggle/select/number/xy_pad/
// envelope_editor/meter/label/group/image/sprite) renders natively in
// ImGui inside the plugin's workspace window; plugins without a usable
// control tree (or that only declared a webview on the web side) get
// the auto-generated parameter panel, the same fallback external
// plugin hosting uses.
//
// Image/sprite assets come from the plugin archive and upload lazily
// into GL textures cached per (plugin id, asset). Sprites with a frame
// grid animate on ImGui time (web pluginSprite.ts semantics: 10 fps
// default, loop animations idle, one-shots fired by controls carrying
// an `animation` interaction key); sprites without animations stay on
// frame 0.
#pragma once

#include "app/project_session.h"
#include "ui/theme.h"

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

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

    // Session + workspace reach the leaves for the interactions that
    // outgrow the instance: envelope-stage commits (structural, via
    // the session) and sprite triggers (state keyed per workspace).
    void draw_control(app::ProjectSession& session, const std::string& workspace_id,
                      plugins::NtpInstance& instance, const ntp::Manifest& manifest,
                      const plugins::LoadedNtpPlugin& plugin, const ntp::UiControl& control,
                      const Theme& theme);
    void draw_envelope_editor(app::ProjectSession& session, const std::string& workspace_id,
                              const ntp::Manifest& manifest, const ntp::UiControl& control,
                              const Theme& theme);
    void draw_sprite(const std::string& workspace_id, const plugins::LoadedNtpPlugin& plugin,
                     const ntp::UiControl& control);
    // Queues the control's `animation` one-shot when its widget was
    // just activated; the owning sprite consumes it when it draws.
    void queue_animation(const std::string& workspace_id, const ntp::UiControl& control);
    static void draw_auto_panel(plugins::NtpInstance& instance, const ntp::Manifest& manifest);
    static void draw_param_widget(plugins::NtpInstance& instance, const ntp::ParamDef& def,
                                  bool as_knob, float width_hint);
    // User-assignable sample slots: current assignment, load-by-path,
    // clear-to-fallback. Rendered under the control tree whenever the
    // manifest declares slots.
    void draw_slot_picker(app::ProjectSession& session, const std::string& workspace_id,
                          plugins::NtpInstance& instance, const Theme& theme);

    std::map<std::string, Texture> textures_; // "<plugin id>/<asset>"

    // Slot picker state, keyed "<workspace id>/<slot id>" — one path
    // field and last status per slot, persistent across frames.
    struct SlotPickerState {
        std::array<char, 512> path{};
        std::string status;
    };

    std::map<std::string, SlotPickerState> slot_state_;

    // Sprite one-shot playback, keyed "<workspace id>/<first animation
    // name>" — animation names are plugin-unique (loader-validated),
    // so a sprite's first name identifies it. Loop/idle playback is
    // stateless (pure function of ImGui time).
    struct SpriteState {
        int anim = -1;      // index into the control's animations
        double start = 0.0; // ImGui time at trigger
    };

    std::map<std::string, SpriteState> sprite_state_;
    // Fired interaction keys, "<workspace id>/<animation name>",
    // consumed by the owning sprite on its next draw (same frame or
    // the one after, depending on control order).
    std::set<std::string> pending_triggers_;

    // The one live envelope drag: stage points preview locally while
    // the mouse is down; release commits through the session (one
    // structural republish per drag).
    struct EnvDrag {
        std::string key; // "<workspace id>/<node id>"; empty = idle
        int stage = -1;
        double total = 0.0; // frozen time-axis scale for the drag
        std::vector<ntp::EnvelopeStage> preview;
    };

    EnvDrag env_drag_;
};

} // namespace nt::ui
