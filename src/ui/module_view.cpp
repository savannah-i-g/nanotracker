#include "ui/module_view.h"

#include <fstream>
#include <imgui.h>
#include <vector>

namespace nt::ui {

bool ModuleView::load_file(audio::AudioEngine& audio, modplay::ModulePlayer& player,
                           const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        status_ = "cannot open " + path;
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    // Detach before touching the player: the audio thread must not
    // render a module that is being replaced.
    audio.send({.type = audio::Command::Type::kSetModule, .module = nullptr});
    if (!player.load(bytes.data(), bytes.size())) {
        status_ = player.error();
        return false;
    }
    audio.send({.type = audio::Command::Type::kSetModule, .module = &player});
    status_ = player.format() + " — " + (player.title().empty() ? "(untitled)" : player.title());
    return true;
}

void ModuleView::draw(audio::AudioEngine& audio, modplay::ModulePlayer& player,
                      const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(430, 200), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MODULE PLAYER")) {
        // Player only: faithful playback of the raw module. Editing a
        // module means converting it to a project — FILE → import module.
        ImGui::TextColored(theme.text_dim, "player only - FILE > import module converts to a "
                                           "project");
        ImGui::InputTextWithHint("##modpath", "path/to/module.mod", path_buf_.data(),
                                 path_buf_.size());
        ImGui::SameLine();
        if (ImGui::Button("LOAD")) {
            load_file(audio, player, path_buf_.data());
        }
        if (!status_.empty()) {
            ImGui::TextColored(theme.text, "%s", status_.c_str());
        }

        if (player.loaded()) {
            const bool playing = audio.snapshot().module_playing;
            if (ImGui::Button(playing ? "STOP" : "PLAY")) {
                audio.send({.type = playing ? audio::Command::Type::kModuleStop
                                            : audio::Command::Type::kModulePlay});
            }
            ImGui::SameLine();
            ImGui::TextColored(theme.text_dim, "ORD %02d ROW %02d", player.current_order(),
                               player.current_row());

            auto position = static_cast<float>(player.position_seconds());
            const float duration = std::max(0.01F, static_cast<float>(player.duration_seconds()));
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::SliderFloat("##pos", &position, 0.0F, duration, "%.1fs")) {
                player.seek_seconds(position);
            }
        }
    }
    ImGui::End();
}

} // namespace nt::ui
