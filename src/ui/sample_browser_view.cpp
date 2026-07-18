#include "ui/sample_browser_view.h"

#include "audio/sample_buffer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <system_error>

namespace nt::ui {

namespace {

// Formats audio/load_sample_memory actually decodes (decoders.cpp
// wraps dr_wav, stb_vorbis and dr_mp3 — no FLAC decoder is vendored).
constexpr std::array<const char*, 3> kAudioExtensions = {".wav", ".ogg", ".mp3"};

bool has_audio_extension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(kAudioExtensions.begin(), kAudioExtensions.end(),
                       [&ext](const char* candidate) { return ext == candidate; });
}

// Browsing starts at the user's home directory; platform/paths.h
// deliberately knows only config and asset locations, so the browser
// resolves home itself (HOME on POSIX, USERPROFILE on Windows).
std::filesystem::path start_directory() {
    for (const char* var : {"HOME", "USERPROFILE"}) {
        const char* value = std::getenv(var);
        if (value != nullptr && *value != '\0') {
            std::error_code ec;
            if (std::filesystem::is_directory(value, ec)) {
                return value;
            }
        }
    }
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path("/") : cwd;
}

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

} // namespace

SampleBrowserView::SampleBrowserView() : dir_(start_directory()) {}

void SampleBrowserView::navigate_to(const std::filesystem::path& dir) {
    dir_ = dir;
    selected_.clear();
    dirty_ = true;
}

void SampleBrowserView::refresh() {
    entries_.clear();
    list_error_.clear();
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(
        dir_, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        list_error_ = ec.message();
        return;
    }
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code entry_ec;
        const bool is_dir = it->is_directory(entry_ec);
        if (entry_ec) {
            continue; // unreadable entry (broken symlink, race): skip
        }
        const std::filesystem::path& path = it->path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.') {
            continue; // hidden entries would bury the audio files
        }
        if (!is_dir && !has_audio_extension(path)) {
            continue;
        }
        Entry entry{.path = path, .name = name, .bytes = 0, .is_dir = is_dir};
        if (!is_dir) {
            entry.bytes = it->file_size(entry_ec);
            if (entry_ec) {
                entry.bytes = 0;
            }
        }
        entries_.push_back(std::move(entry));
    }
    // Directories first, then files; case-insensitive name order within
    // each group.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir;
        }
        const auto less = [](unsigned char l, unsigned char r) {
            return std::tolower(l) < std::tolower(r);
        };
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(),
                                            b.name.end(), less);
    });
}

void SampleBrowserView::audition(audio::AudioEngine& audio,
                                 std::vector<std::unique_ptr<audio::SampleBuffer>>& pool,
                                 const Entry& entry) {
    if (!audio.running()) {
        status_ = "audio device not running";
        return;
    }
    const std::string key = entry.path.string();
    const audio::SampleBuffer* buffer = nullptr;
    if (const auto found = decoded_.find(key); found != decoded_.end()) {
        buffer = found->second;
    } else {
        std::ifstream file(entry.path, std::ios::binary);
        if (!file) {
            status_ = "cannot open " + key;
            return;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                              std::istreambuf_iterator<char>());
        std::string format;
        std::string error;
        auto loaded = audio::load_sample_memory(bytes.data(), bytes.size(), audio.sample_rate(),
                                                format, error);
        if (loaded == nullptr) {
            status_ = error;
            return;
        }
        buffer = loaded.get();
        pool.push_back(std::move(loaded));
        decoded_.emplace(key, buffer);
    }
    // Same one-shot preview voice as the shell's load & play; a new
    // audition replaces the previous one.
    audio.send({.type = audio::Command::Type::kPlaySample, .value = 0.0F, .sample = buffer});
    status_ = entry.name + " — " + std::to_string(buffer->frames) + " fr @ " +
              std::to_string(buffer->rate) + " Hz";
}

void SampleBrowserView::draw(app::ProjectSession& session, audio::AudioEngine& audio,
                             std::vector<std::unique_ptr<audio::SampleBuffer>>& audition_pool,
                             int target_slot, const Theme& theme) {
    ImGui::SetNextWindowSize(ImVec2(430, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("SAMPLE BROWSER")) {
        if (dirty_) {
            refresh();
            dirty_ = false;
        }

        const bool has_parent = dir_.has_parent_path() && dir_ != dir_.root_path();
        ImGui::BeginDisabled(!has_parent);
        if (ImGui::Button("UP")) {
            navigate_to(dir_.parent_path());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("RESCAN")) {
            dirty_ = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(theme.text_dim, "dbl-click loads into slot %02X", target_slot);
        ImGui::TextWrapped("%s", dir_.string().c_str());
        ImGui::Separator();

        if (ImGui::BeginChild("entries", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_Borders)) {
            if (!list_error_.empty()) {
                ImGui::TextColored(theme.text_dim, "%s", list_error_.c_str());
            } else if (entries_.empty()) {
                ImGui::TextColored(theme.text_dim, "no folders or audio files (%s)", "wav/ogg/mp3");
            }
            for (const Entry& entry : entries_) {
                std::array<char, 64> label{};
                if (entry.is_dir) {
                    std::snprintf(label.data(), label.size(), "/%s", entry.name.c_str());
                } else {
                    std::snprintf(label.data(), label.size(), "%-38.38s %10s", entry.name.c_str(),
                                  format_size(entry.bytes).c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Text, entry.is_dir ? theme.text_dim : theme.text);
                const bool clicked = ImGui::Selectable(label.data(), entry.path == selected_,
                                                       ImGuiSelectableFlags_AllowDoubleClick);
                ImGui::PopStyleColor();
                if (!clicked) {
                    continue;
                }
                selected_ = entry.path;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (entry.is_dir) {
                        navigate_to(entry.path);
                    } else {
                        status_ =
                            session.load_sample_into_slot(target_slot, entry.path)
                                ? entry.name + " loaded into slot " + std::to_string(target_slot)
                                : session.error();
                    }
                } else if (!entry.is_dir) {
                    audition(audio, audition_pool, entry);
                }
            }
        }
        ImGui::EndChild();
        if (!status_.empty()) {
            ImGui::TextColored(theme.text_dim, "%s", status_.c_str());
        }
    }
    ImGui::End();
}

} // namespace nt::ui
