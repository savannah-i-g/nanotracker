#include "ui/local_api_view.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <imgui.h>
#include <ranges>
#include <string>

namespace nt::ui {

namespace {

// Web activity-log red (#c55) for denied/error rows.
constexpr ImVec4 kDeniedColor{0.8F, 0.33F, 0.33F, 1.0F};

std::string format_time(std::chrono::system_clock::time_point when) {
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::array<char, 16> buf{};
    std::strftime(buf.data(), buf.size(), "%H:%M:%S", &tm_buf);
    return buf.data();
}

} // namespace

void LocalApiView::apply_enabled(api::LocalApiServer& server, io::Settings& settings) {
    if (!settings.local_api_enabled) {
        server.stop();
        return;
    }
    if (settings.local_api_token.empty()) {
        settings.local_api_token = api::generate_token();
    }
    if (!server.start(settings.local_api_port, settings.local_api_token)) {
        // The checkbox must reflect reality: a failed bind reads as
        // disabled, with the error shown in the status banner.
        settings.local_api_enabled = false;
    }
}

void LocalApiView::draw(api::LocalApiServer& server, io::Settings& settings, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(470, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LOCAL API")) {
        ImGui::End();
        return;
    }

    // ── Status banner ────────────────────────────────────────────────
    if (server.running()) {
        ImGui::TextColored(theme.primary, "ENABLED — ws://127.0.0.1:%d", server.port());
    } else {
        ImGui::TextColored(theme.text_dim, "DISABLED — no remote access");
        if (!server.error().empty()) {
            ImGui::TextColored(kDeniedColor, "last error: %s", server.error().c_str());
        }
    }
    ImGui::Separator();

    // ── Enable / port / token ────────────────────────────────────────
    bool enabled = settings.local_api_enabled;
    if (ImGui::Checkbox("enable local API", &enabled)) {
        settings.local_api_enabled = enabled;
        apply_enabled(server, settings);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("localhost WebSocket, bearer-token auth");

    ImGui::BeginDisabled(server.running());
    int port = settings.local_api_port;
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::InputInt("port", &port)) {
        settings.local_api_port = std::clamp(port, 1024, 65535);
    }
    ImGui::EndDisabled();
    if (server.running()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(disable to change)");
    }

    std::array<char, 64> token_buf{};
    std::snprintf(token_buf.data(), token_buf.size(), "%s", settings.local_api_token.c_str());
    ImGui::SetNextItemWidth(260.0F);
    ImGui::InputText("token", token_buf.data(), token_buf.size(), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::SmallButton("copy")) {
        ImGui::SetClipboardText(settings.local_api_token.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("regenerate")) {
        settings.local_api_token = api::generate_token();
        if (server.running()) {
            // Existing connections authed with the old token drop.
            server.stop();
            apply_enabled(server, settings);
        }
    }

    ImGui::SeparatorText("clients");
    draw_clients(server, theme);

    ImGui::SeparatorText("requests");
    draw_log(server, theme);

    ImGui::End();
}

void LocalApiView::draw_clients(const api::LocalApiServer& server, const Theme& theme) {
    const std::vector<api::ClientInfo> clients = server.clients();
    if (clients.empty()) {
        ImGui::TextColored(theme.text_dim, "no clients connected.");
        return;
    }
    if (ImGui::BeginTable("clients", 4, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("address", ImGuiTableColumnFlags_WidthFixed, 140.0F);
        ImGui::TableSetupColumn("connected", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("auth", ImGuiTableColumnFlags_WidthFixed, 44.0F);
        ImGui::TableSetupColumn("requests");
        ImGui::TableHeadersRow();
        for (const api::ClientInfo& client : clients) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(client.address.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_time(client.connected_at).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(client.authed ? "yes" : "—");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(client.requests));
        }
        ImGui::EndTable();
    }
}

void LocalApiView::draw_log(api::LocalApiServer& server, const Theme& theme) {
    const std::vector<api::LogEntry> log = server.log_tail();
    if (ImGui::SmallButton("clear log")) {
        server.clear_log();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d entries", static_cast<int>(log.size()));
    if (ImGui::BeginChild("log", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders)) {
        // Newest first, like the web activity log.
        for (const api::LogEntry& entry : std::ranges::reverse_view(log)) {
            const bool denied = entry.kind == "denied" || entry.kind == "error" || !entry.ok;
            ImGui::TextColored(theme.text_dim, "%s", format_time(entry.time).c_str());
            ImGui::SameLine();
            ImGui::TextColored(denied ? kDeniedColor : theme.primary, "%-7s", entry.kind.c_str());
            ImGui::SameLine();
            ImGui::TextColored(theme.text, "%s", entry.description.c_str());
        }
        if (log.empty()) {
            ImGui::TextColored(theme.text_dim, "no activity yet.");
        }
    }
    ImGui::EndChild();
}

} // namespace nt::ui
