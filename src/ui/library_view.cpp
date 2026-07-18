#include "ui/library_view.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <imgui.h>
#include <system_error>

namespace nt::ui {

namespace {

// Bounds on the recursive walk: a library root is a real directory tree,
// but the browser must never block the UI thread walking an unbounded
// one. Depth and total-entry caps keep a rescan finite; the listing is
// cached and only rebuilt on a rescan or root change.
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxAssets = 4000;

std::string format_size(std::uintmax_t bytes) {
    std::array<char, 24> text{};
    if (bytes < 1024) {
        std::snprintf(text.data(), text.size(), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(text.data(), text.size(), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text.data(), text.size(), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return text.data();
}

std::size_t roots_signature(const io::Settings& settings) {
    std::size_t seed = settings.library_roots.size();
    for (const std::string& root : settings.library_roots) {
        // Boost-style hash combine: order-sensitive, so reordering roots
        // also triggers a rescan.
        seed ^= std::hash<std::string>{}(root) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    }
    return seed;
}

} // namespace

void LibraryView::rescan(const io::Settings& settings) {
    assets_.clear();
    for (const std::string& root : settings.library_roots) {
        std::error_code ec;
        auto it = std::filesystem::recursive_directory_iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
            continue; // unreadable root (unmounted / permissions): skip
        }
        const std::filesystem::path root_path(root);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (assets_.size() >= kMaxAssets) {
                return;
            }
            const std::filesystem::path& path = it->path();
            const std::string name = path.filename().string();
            if (name.empty() || name.front() == '.') {
                it.disable_recursion_pending(); // skip hidden files and trees
                continue;
            }
            std::error_code entry_ec;
            if (it->is_directory(entry_ec)) {
                if (it.depth() >= kMaxDepth) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const AssetKind kind = classify_asset(path);
            if (kind == AssetKind::kOther) {
                continue;
            }
            Asset asset;
            asset.path = path;
            asset.label = std::filesystem::relative(path, root_path, entry_ec).string();
            if (entry_ec || asset.label.empty()) {
                asset.label = name;
            }
            asset.kind = kind;
            asset.bytes = it->file_size(entry_ec);
            if (entry_ec) {
                asset.bytes = 0;
            }
            assets_.push_back(std::move(asset));
        }
    }
    std::sort(assets_.begin(), assets_.end(), [](const Asset& a, const Asset& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        const auto less = [](unsigned char l, unsigned char r) {
            return std::tolower(l) < std::tolower(r);
        };
        return std::lexicographical_compare(a.label.begin(), a.label.end(), b.label.begin(),
                                            b.label.end(), less);
    });
}

void LibraryView::audition(app::ProjectSession& session, const std::filesystem::path& path) {
    if (!session.preview_file(path)) {
        status_ = session.error();
        return;
    }
    const audio::SampleBuffer* buffer = session.preview_buffer();
    status_ = buffer != nullptr
                  ? path.filename().string() + " — " + std::to_string(buffer->frames) + " fr @ " +
                        std::to_string(buffer->rate) + " Hz"
                  : path.filename().string();
}

void LibraryView::install(app::ProjectSession& session, const std::filesystem::path& path) {
    const std::string plugin_id = session.load_plugin_file(path);
    status_ = plugin_id.empty() ? "install failed: " + session.error()
                                : "installed \"" + plugin_id + "\" into the catalogue";
}

void LibraryView::draw_asset_row(app::ProjectSession& session, io::Settings& settings,
                                 const Theme& theme, const std::filesystem::path& path,
                                 const std::string& label, AssetKind kind) {
    const std::string key = path.string();
    ImGui::PushID(key.c_str());
    const bool favourited =
        std::find(settings.favourite_paths.begin(), settings.favourite_paths.end(), key) !=
        settings.favourite_paths.end();
    if (ImGui::SmallButton(favourited ? "*" : "o")) {
        toggle_favourite(settings.favourite_paths, key);
    }
    ImGui::SetItemTooltip(favourited ? "unfavourite" : "favourite");
    ImGui::SameLine();

    if (kind == AssetKind::kSample) {
        if (ImGui::SmallButton("audition")) {
            audition(session, path);
        }
    } else if (kind == AssetKind::kNtpArchive) {
        if (ImGui::SmallButton("install")) {
            install(session, path);
        }
    }
    ImGui::SameLine();

    std::error_code ec;
    const bool missing = !std::filesystem::exists(path, ec) || ec;
    ImGui::TextColored(missing ? theme.text_dim : theme.text, "%s%s", label.c_str(),
                       missing ? "  (missing)" : "");
    ImGui::SetItemTooltip("%s", key.c_str());
    ImGui::PopID();
}

void LibraryView::draw(app::ProjectSession& session, io::Settings& settings, const Theme& theme,
                       bool& open) {
    ImGui::SetNextWindowSize(ImVec2(460, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LIBRARY", &open)) {
        ImGui::End();
        return;
    }

    // ── Roots ────────────────────────────────────────────────────────
    ImGui::TextColored(theme.text_dim, "library roots");
    ImGui::SetNextItemWidth(-90.0F);
    const bool entered =
        ImGui::InputTextWithHint("##newroot", "path to a library folder", new_root_.data(),
                                 new_root_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("add root") || entered) && new_root_[0] != '\0') {
        const std::string root = new_root_.data();
        if (std::find(settings.library_roots.begin(), settings.library_roots.end(), root) ==
            settings.library_roots.end()) {
            settings.library_roots.push_back(root);
            dirty_ = true;
        }
        new_root_[0] = '\0';
    }

    int remove_root = -1;
    for (int i = 0; i < static_cast<int>(settings.library_roots.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("x")) {
            remove_root = i;
        }
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim, "%s",
                           settings.library_roots[static_cast<std::size_t>(i)].c_str());
        ImGui::PopID();
    }
    if (remove_root >= 0) {
        settings.library_roots.erase(settings.library_roots.begin() + remove_root);
        dirty_ = true;
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("rescan")) {
        dirty_ = true;
    }

    // A root add/remove elsewhere (or the very first frame) triggers a
    // rebuild; the cached listing otherwise survives across frames.
    if (const std::size_t signature = roots_signature(settings); signature != roots_signature_) {
        roots_signature_ = signature;
        dirty_ = true;
    }
    if (dirty_) {
        rescan(settings);
        dirty_ = false;
    }
    ImGui::Separator();

    if (ImGui::BeginChild("library", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                          ImGuiChildFlags_Borders)) {
        // ── Favourites ───────────────────────────────────────────────
        // Each section gets its own ImGui id scope: a favourited asset
        // also appears in its kind's listing, and the per-row PushID(path)
        // would otherwise collide across the two.
        if (ImGui::CollapsingHeader("favourites", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("favourites");
            if (settings.favourite_paths.empty()) {
                ImGui::TextColored(theme.text_dim, "no favourites — star a sample or archive");
            }
            // Iterate a copy: the star toggle mutates the real vector.
            const std::vector<std::string> favourites = settings.favourite_paths;
            for (const std::string& fav : favourites) {
                const std::filesystem::path path(fav);
                draw_asset_row(session, settings, theme, path, path.filename().string(),
                               classify_asset(path));
            }
            ImGui::PopID();
        }

        // ── Samples / NTP archives ───────────────────────────────────
        const auto section = [&](const char* title, AssetKind kind) {
            std::size_t count = 0;
            for (const Asset& asset : assets_) {
                count += asset.kind == kind ? 1 : 0;
            }
            std::array<char, 48> header{};
            std::snprintf(header.data(), header.size(), "%s (%zu)", title, count);
            if (!ImGui::CollapsingHeader(header.data(), ImGuiTreeNodeFlags_DefaultOpen)) {
                return;
            }
            if (count == 0) {
                ImGui::TextColored(theme.text_dim, "none under the current roots");
                return;
            }
            ImGui::PushID(title);
            for (const Asset& asset : assets_) {
                if (asset.kind != kind) {
                    continue;
                }
                draw_asset_row(session, settings, theme, asset.path,
                               asset.label + "  (" + format_size(asset.bytes) + ")", asset.kind);
            }
            ImGui::PopID();
        };
        section("samples", AssetKind::kSample);
        section("ntp archives", AssetKind::kNtpArchive);

        ImGui::Spacing();
        ImGui::TextColored(theme.text_dim,
                           "presets live per-plugin in the config directory — applied from the "
                           "plugin panel, not browsed here.");
    }
    ImGui::EndChild();

    if (!status_.empty()) {
        ImGui::TextColored(theme.text_dim, "%s", status_.c_str());
    }
    ImGui::End();
}

} // namespace nt::ui
