#include "ui/projects_view.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <imgui.h>
#include <system_error>
#include <utility>

namespace nt::ui {

namespace {

// Absolute, normalised form for stable identity/dedup. weakly_canonical
// resolves symlinks and "." / ".." where the path exists; a missing
// target (unmounted drive) falls back to a lexically-normalised absolute
// path so the entry still round-trips.
std::filesystem::path canonical_key(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec || resolved.empty()) {
        resolved = std::filesystem::absolute(path, ec).lexically_normal();
    }
    return resolved;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Compact local-time stamp for the "when" column; empty when the epoch
// is unknown (0) or localtime refuses the value.
std::string format_when(std::int64_t seconds) {
    if (seconds <= 0) {
        return {};
    }
    const auto t = static_cast<std::time_t>(seconds);
    std::tm tm_buf{};
#if defined(_WIN32)
    if (localtime_s(&tm_buf, &t) != 0) {
        return {};
    }
#else
    if (localtime_r(&t, &tm_buf) == nullptr) {
        return {};
    }
#endif
    std::array<char, 20> text{};
    if (std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M", &tm_buf) == 0) {
        return {};
    }
    return text.data();
}

} // namespace

void ProjectsView::record(io::Settings& settings, const app::ProjectSession& session,
                          const std::filesystem::path& path) {
    const engine::TrackerProject& project = session.project();
    io::RecentProject entry;
    entry.path = canonical_key(path).string();
    entry.name = project.name;
    entry.channels = project.channels;
    entry.patterns = static_cast<int>(project.patterns.size());
    entry.last_opened = now_seconds();
    record_recent_project(settings.recent_projects, std::move(entry), kUnpinnedRecentCap);
}

void ProjectsView::draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme,
                        bool& open) {
    ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PROJECTS", &open)) {
        ImGui::End();
        return;
    }

    // ── New from template ────────────────────────────────────────────
    ImGui::TextColored(theme.text_dim, "new from template");
    if (ImGui::Button("4ch empty")) {
        session.new_project(4);
        status_ = "new 4-channel project";
    }
    ImGui::SameLine();
    if (ImGui::Button("8ch empty")) {
        session.new_project(8);
        status_ = "new 8-channel project";
    }
    ImGui::Separator();

    std::vector<io::RecentProject>& recents = settings.recent_projects;
    sort_recent_projects(recents);

    if (ImGui::BeginChild("recents", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                          ImGuiChildFlags_Borders)) {
        if (recents.empty()) {
            ImGui::TextColored(theme.text_dim,
                               "no recent projects — open or save a project to populate this list");
        }
        // Index-stable erase target: a missing-entry remove is applied
        // after the loop so the vector is not reshaped mid-iteration.
        int remove_index = -1;
        int open_index = -1;
        int pin_index = -1;
        for (int i = 0; i < static_cast<int>(recents.size()); ++i) {
            const io::RecentProject& entry = recents[static_cast<std::size_t>(i)];
            std::error_code ec;
            const bool missing = !std::filesystem::exists(entry.path, ec) || ec;

            ImGui::PushID(i);
            // Pin toggle: a filled marker when pinned, hollow otherwise.
            if (ImGui::SmallButton(entry.pinned ? "*" : "o")) {
                pin_index = i;
            }
            ImGui::SetItemTooltip(entry.pinned ? "unpin" : "pin (keeps this project at the top)");
            ImGui::SameLine();

            std::array<char, 96> meta{};
            const std::string when = format_when(entry.last_opened);
            std::snprintf(meta.data(), meta.size(), "%dch / %dpat%s%s", entry.channels,
                          entry.patterns, when.empty() ? "" : "  ", when.c_str());

            const std::string label =
                (entry.name.empty() ? std::string("(untitled)") : entry.name) + "  —  " +
                meta.data();
            ImGui::PushStyleColor(ImGuiCol_Text, missing ? theme.text_dim : theme.text);
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                !missing) {
                open_index = i;
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", entry.path.c_str());

            if (missing) {
                ImGui::SameLine();
                ImGui::TextColored(theme.text_dim, "(missing)");
                ImGui::SameLine();
                if (ImGui::SmallButton("remove")) {
                    remove_index = i;
                }
                ImGui::SetItemTooltip("forget this entry (does not delete the file)");
            }
            ImGui::PopID();
        }

        // Deferred mutations, one per frame (a single click can only hit
        // one control), applied outside the render loop.
        if (pin_index >= 0) {
            toggle_recent_pin(recents, recents[static_cast<std::size_t>(pin_index)].path);
        } else if (remove_index >= 0) {
            recents.erase(recents.begin() + remove_index);
        } else if (open_index >= 0) {
            const std::filesystem::path path = recents[static_cast<std::size_t>(open_index)].path;
            if (session.load_file(path)) {
                record(settings, session, path); // refresh the metadata cache
                status_ = "opened " + path.filename().string();
            } else {
                status_ = session.error();
            }
        }
    }
    ImGui::EndChild();

    if (!status_.empty()) {
        ImGui::TextColored(theme.text_dim, "%s", status_.c_str());
    }
    ImGui::End();
}

} // namespace nt::ui
