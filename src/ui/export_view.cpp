#include "ui/export_view.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <imgui.h>
#include <utility>

namespace nt::ui {

namespace {

// Bounded copy into a fixed form buffer (truncates, always terminated).
template <std::size_t N>
void copy_text(std::array<char, N>& buf, const std::string& text) {
    std::snprintf(buf.data(), buf.size(), "%s", text.c_str());
}

const char* format_label(io::ExportFormat format) {
    switch (format) {
    case io::ExportFormat::kOgg:
        return "OGG";
    case io::ExportFormat::kMp3:
        return "MP3";
    case io::ExportFormat::kWav:
        break;
    }
    return "WAV";
}

const char* format_extension(io::ExportFormat format) {
    switch (format) {
    case io::ExportFormat::kOgg:
        return ".ogg";
    case io::ExportFormat::kMp3:
        return ".mp3";
    case io::ExportFormat::kWav:
        break;
    }
    return ".wav";
}

const char* shape_label(io::FadeShape shape) {
    return shape == io::FadeShape::kEqualPower ? "equal-power" : "linear";
}

const char* normalize_label(io::NormalizeMode mode) {
    switch (mode) {
    case io::NormalizeMode::kPeak:
        return "PEAK";
    case io::NormalizeMode::kTruePeak:
        return "TRUE-PEAK";
    case io::NormalizeMode::kLufs:
        return "LUFS";
    case io::NormalizeMode::kNone:
        break;
    }
    return "OFF";
}

// Target unit per mode (io/export_post.h: dBFS / dBTP / LUFS).
const char* normalize_unit(io::NormalizeMode mode) {
    switch (mode) {
    case io::NormalizeMode::kTruePeak:
        return "dBTP";
    case io::NormalizeMode::kLufs:
        return "LUFS";
    case io::NormalizeMode::kPeak:
    case io::NormalizeMode::kNone:
        break;
    }
    return "dBFS";
}

// Two-value combo for a fade shape; returns true on change.
bool shape_combo(const char* id, io::FadeShape& shape) {
    bool changed = false;
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::BeginCombo(id, shape_label(shape))) {
        for (const io::FadeShape option : {io::FadeShape::kLinear, io::FadeShape::kEqualPower}) {
            if (ImGui::Selectable(shape_label(option), option == shape)) {
                changed = shape != option;
                shape = option;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

ExportView::ExportView(std::filesystem::path preset_file)
    : presets_(std::move(preset_file)), preset_id_(io::builtin_export_presets().front().id),
      options_(io::builtin_export_presets().front().options),
      end_order_last_(options_.end_order < 0) {}

ExportView::~ExportView() {
    if (worker_.joinable()) {
        worker_.join(); // an in-flight render finishes; shutdown waits
    }
}

void ExportView::prefill_defaults(const app::ProjectSession& session) {
    if (defaults_filled_) {
        return;
    }
    defaults_filled_ = true;
    const std::string& name = session.project().name;
    std::snprintf(path_buf_.data(), path_buf_.size(), "%s%s", name.c_str(),
                  format_extension(options_.format));
    copy_text(meta_title_, name);
    // Year prefill, as the web modal did.
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::snprintf(meta_date_.data(), meta_date_.size(), "%04d", local.tm_year + 1900);
}

void ExportView::apply_preset(const io::ExportPreset& preset) {
    preset_id_ = preset.id;
    options_ = preset.options;
    end_order_last_ = options_.end_order < 0;
    preset_dirty_ = false;
    // Metadata rides in the preset; empty fields keep the form blank.
    copy_text(meta_title_, options_.metadata.title);
    copy_text(meta_artist_, options_.metadata.artist);
    copy_text(meta_album_, options_.metadata.album);
    copy_text(meta_date_, options_.metadata.date);
    copy_text(meta_comment_, options_.metadata.comment);
    set_path_extension_for_format();
}

void ExportView::set_path_extension_for_format() {
    if (path_buf_[0] == '\0') {
        return;
    }
    std::filesystem::path path = path_buf_.data();
    path.replace_extension(format_extension(options_.format));
    const std::string text = path.string();
    if (text.size() < path_buf_.size()) {
        copy_text(path_buf_, text);
    }
}

void ExportView::draw_preset_row() {
    const io::ExportPreset* selected = presets_.find(preset_id_);
    std::string label = selected != nullptr ? selected->name : "custom";
    if (preset_dirty_) {
        label += " *";
    }
    ImGui::SetNextItemWidth(240.0F);
    if (ImGui::BeginCombo("##preset", label.c_str())) {
        for (const io::ExportPreset& preset : io::builtin_export_presets()) {
            ImGui::PushID(preset.id.c_str());
            if (ImGui::Selectable(preset.name.c_str(), preset.id == preset_id_)) {
                apply_preset(preset);
            }
            ImGui::PopID();
        }
        if (!presets_.user_presets().empty()) {
            ImGui::Separator();
            for (const io::ExportPreset& preset : presets_.user_presets()) {
                ImGui::PushID(preset.id.c_str());
                if (ImGui::Selectable(preset.name.c_str(), preset.id == preset_id_)) {
                    apply_preset(preset);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0F);
    ImGui::InputTextWithHint("##pname", "preset name", preset_name_buf_.data(),
                             preset_name_buf_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("save")) {
        const std::string name = preset_name_buf_.data();
        if (name.empty()) {
            status_ = "name the preset first";
            last_export_ok_ = false;
        } else {
            const std::string id = presets_.save_user_preset(name, snapshot_options());
            if (id.empty()) {
                status_ = "preset save failed (config dir not writable?)";
                last_export_ok_ = false;
            } else {
                preset_id_ = id;
                preset_dirty_ = false;
                status_ = "saved preset: " + name;
                last_export_ok_ = true;
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(io::is_builtin_export_preset(preset_id_));
    if (ImGui::SmallButton("delete")) {
        presets_.delete_user_preset(preset_id_);
        apply_preset(io::builtin_export_presets().front());
    }
    ImGui::EndDisabled();
}

void ExportView::draw_output_section(const app::ProjectSession& session) {
    ImGui::SeparatorText("output");
    ImGui::SetNextItemWidth(340.0F);
    ImGui::InputTextWithHint("path", "path/to/mix.wav", path_buf_.data(), path_buf_.size());
    ImGui::TextDisabled("%s @ %d bpm — %d channels, %d orders", session.project().name.c_str(),
                        session.project().bpm, session.project().channels,
                        static_cast<int>(session.project().order_list.size()));
}

void ExportView::draw_format_section() {
    ImGui::SeparatorText("format");
    bool changed = false;
    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::BeginCombo("format", format_label(options_.format))) {
        for (const io::ExportFormat format :
             {io::ExportFormat::kWav, io::ExportFormat::kOgg, io::ExportFormat::kMp3}) {
            if (ImGui::Selectable(format_label(format), format == options_.format)) {
                changed |= options_.format != format;
                options_.format = format;
                set_path_extension_for_format();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0F);
    static constexpr std::array<std::uint32_t, 6> kRates = {22050, 44100, 48000,
                                                            88200, 96000, 192000};
    std::array<char, 16> rate_label{};
    std::snprintf(rate_label.data(), rate_label.size(), "%u Hz", options_.sample_rate);
    if (ImGui::BeginCombo("rate", rate_label.data())) {
        for (const std::uint32_t rate : kRates) {
            std::snprintf(rate_label.data(), rate_label.size(), "%u Hz", rate);
            if (ImGui::Selectable(rate_label.data(), rate == options_.sample_rate)) {
                changed |= options_.sample_rate != rate;
                options_.sample_rate = rate;
            }
        }
        ImGui::EndCombo();
    }

    if (options_.format == io::ExportFormat::kWav) {
        static constexpr std::array<const char*, 3> kDepths = {"16-bit PCM", "24-bit PCM",
                                                               "32-bit float"};
        ImGui::SetNextItemWidth(120.0F);
        const auto depth_index = static_cast<std::size_t>(options_.wav_depth);
        if (ImGui::BeginCombo("depth", kDepths[std::min<std::size_t>(depth_index, 2)])) {
            for (std::size_t d = 0; d < kDepths.size(); ++d) {
                if (ImGui::Selectable(kDepths[d], d == depth_index)) {
                    changed |= depth_index != d;
                    options_.wav_depth = static_cast<io::WavDepth>(d);
                }
            }
            ImGui::EndCombo();
        }
    } else if (options_.format == io::ExportFormat::kOgg) {
        ImGui::SetNextItemWidth(180.0F);
        changed |= ImGui::SliderFloat("quality", &options_.ogg_quality, -0.1F, 1.0F, "%.2f");
    } else {
        static constexpr std::array<int, 5> kBitrates = {96, 128, 192, 256, 320};
        std::array<char, 16> bitrate_label{};
        std::snprintf(bitrate_label.data(), bitrate_label.size(), "%d kbps",
                      options_.mp3_bitrate_kbps);
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::BeginCombo("bitrate", bitrate_label.data())) {
            for (const int bitrate : kBitrates) {
                std::snprintf(bitrate_label.data(), bitrate_label.size(), "%d kbps", bitrate);
                if (ImGui::Selectable(bitrate_label.data(), bitrate == options_.mp3_bitrate_kbps)) {
                    changed |= options_.mp3_bitrate_kbps != bitrate;
                    options_.mp3_bitrate_kbps = bitrate;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0F);
        changed |= ImGui::SliderInt("quality (0 best)", &options_.mp3_quality, 0, 9);
    }
    preset_dirty_ |= changed;
}

void ExportView::draw_range_section(const app::ProjectSession& session) {
    ImGui::SeparatorText("range");
    const int order_max = std::max(0, static_cast<int>(session.project().order_list.size()) - 1);
    bool changed = false;
    ImGui::SetNextItemWidth(110.0F);
    changed |= ImGui::InputInt("start order", &options_.start_order);
    options_.start_order = std::clamp(options_.start_order, 0, order_max);
    ImGui::SameLine();
    // end_order = -1 means "last position" (the renderer's contract);
    // the checkbox surfaces it so the number field only appears for a
    // real custom end.
    if (ImGui::Checkbox("to last", &end_order_last_)) {
        options_.end_order = end_order_last_ ? -1 : order_max;
        changed = true;
    }
    if (!end_order_last_) {
        ImGui::SetNextItemWidth(110.0F);
        changed |= ImGui::InputInt("end order", &options_.end_order);
        options_.end_order = std::clamp(options_.end_order, options_.start_order, order_max);
    }
    ImGui::SetNextItemWidth(110.0F);
    changed |= ImGui::InputDouble("tail (s)", &options_.tail_seconds, 0.0, 0.0, "%.2f");
    options_.tail_seconds = std::clamp(options_.tail_seconds, 0.0, 30.0);
    preset_dirty_ |= changed;
}

void ExportView::draw_stems_section(const app::ProjectSession& session) {
    ImGui::SeparatorText("stems");
    const int channels = std::min(session.project().channels, engine::kMaxChannels);
    bool changed = false;
    for (int ch = 0; ch < channels; ++ch) {
        if (ch % 8 != 0) {
            ImGui::SameLine();
        }
        std::array<char, 8> label{};
        std::snprintf(label.data(), label.size(), "%02d", ch + 1);
        const auto bit = 1U << static_cast<unsigned>(ch);
        bool on = (options_.stem_mask & bit) != 0;
        if (ImGui::Checkbox(label.data(), &on)) {
            options_.stem_mask = on ? options_.stem_mask | bit : options_.stem_mask & ~bit;
            changed = true;
        }
    }
    ImGui::BeginDisabled(options_.stem_mask == 0);
    changed |= ImGui::Checkbox("bundle stems as zip", &options_.stem_zip);
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", options_.stem_mask == 0
                                  ? "no channels checked — single mix renders to the path"
                                  : "one file per checked channel (<path>.chNN.<ext>)");
    preset_dirty_ |= changed;
}

void ExportView::draw_post_section() {
    ImGui::SeparatorText("post");
    bool changed = false;
    ImGui::SetNextItemWidth(110.0F);
    changed |= ImGui::InputDouble("fade in (s)", &options_.post.fade_in_seconds, 0.0, 0.0, "%.2f");
    ImGui::SameLine();
    changed |= shape_combo("##fadein_shape", options_.post.fade_in_shape);
    ImGui::SetNextItemWidth(110.0F);
    changed |=
        ImGui::InputDouble("fade out (s)", &options_.post.fade_out_seconds, 0.0, 0.0, "%.2f");
    ImGui::SameLine();
    changed |= shape_combo("##fadeout_shape", options_.post.fade_out_shape);
    options_.post.fade_in_seconds = std::clamp(options_.post.fade_in_seconds, 0.0, 60.0);
    options_.post.fade_out_seconds = std::clamp(options_.post.fade_out_seconds, 0.0, 60.0);

    ImGui::SetNextItemWidth(120.0F);
    if (ImGui::BeginCombo("normalize", normalize_label(options_.post.normalize))) {
        for (const io::NormalizeMode mode :
             {io::NormalizeMode::kNone, io::NormalizeMode::kPeak, io::NormalizeMode::kTruePeak,
              io::NormalizeMode::kLufs}) {
            if (ImGui::Selectable(normalize_label(mode), mode == options_.post.normalize)) {
                changed |= options_.post.normalize != mode;
                options_.post.normalize = mode;
            }
        }
        ImGui::EndCombo();
    }
    if (options_.post.normalize != io::NormalizeMode::kNone) {
        std::array<char, 24> label{};
        std::snprintf(label.data(), label.size(), "target (%s)",
                      normalize_unit(options_.post.normalize));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0F);
        changed |=
            ImGui::InputDouble(label.data(), &options_.post.normalize_target_db, 0.0, 0.0, "%.1f");
    }
    preset_dirty_ |= changed;
}

void ExportView::draw_metadata_section() {
    ImGui::SeparatorText("metadata");
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("title", meta_title_.data(), meta_title_.size());
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("artist", meta_artist_.data(), meta_artist_.size());
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("album", meta_album_.data(), meta_album_.size());
    ImGui::SetNextItemWidth(110.0F);
    ImGui::InputText("date", meta_date_.data(), meta_date_.size());
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("comment", meta_comment_.data(), meta_comment_.size());
}

io::ExportOptions ExportView::snapshot_options() {
    io::ExportOptions options = options_;
    options.metadata.title = meta_title_.data();
    options.metadata.artist = meta_artist_.data();
    options.metadata.album = meta_album_.data();
    options.metadata.date = meta_date_.data();
    options.metadata.comment = meta_comment_.data();
    return options;
}

void ExportView::launch_export(app::ProjectSession& session) {
    if (worker_.joinable()) {
        return;
    }
    const std::string path_text = path_buf_.data();
    if (path_text.empty()) {
        status_ = "set an output path first";
        last_export_ok_ = false;
        return;
    }
    session.stop();

    // The renderer needs the project plus its write extras (bundled
    // plugins, workspace, presets, POVR) so the offline session matches
    // a load. Both snapshot here on the UI thread as value copies; the
    // worker never touches the session.
    io::FtrkWriteExtras extras = session.assemble_write_extras();

    status_.clear();
    last_files_.clear();
    running_.store(true, std::memory_order_release);
    // Captures are initialised here on the UI thread; the worker owns
    // copies only and reports through the guarded result slot.
    worker_ = std::thread([this, project = session.project(), extras = std::move(extras),
                           out = std::filesystem::path(path_text), options = snapshot_options()]() {
        io::ExportResult result = io::export_project(project, extras, out, options);
        {
            const std::lock_guard<std::mutex> lock(result_mutex_);
            result_ = std::move(result);
        }
        running_.store(false, std::memory_order_release);
    });
}

void ExportView::poll_worker() {
    if (!worker_.joinable() || running_.load(std::memory_order_acquire)) {
        return;
    }
    worker_.join();
    io::ExportResult result;
    {
        const std::lock_guard<std::mutex> lock(result_mutex_);
        result = std::move(result_);
    }
    last_export_ok_ = result.ok;
    if (result.ok) {
        std::array<char, 96> line{};
        std::snprintf(line.data(), line.size(), "done — %d file%s, %.2fs rendered",
                      static_cast<int>(result.files.size()), result.files.size() == 1 ? "" : "s",
                      result.seconds);
        status_ = line.data();
        for (const std::filesystem::path& file : result.files) {
            last_files_.push_back(file.filename().string());
        }
    } else {
        status_ = "export failed: " + result.error;
    }
}

void ExportView::draw_status_section(app::ProjectSession& session, const Theme& theme) {
    ImGui::Separator();
    const bool busy = worker_.joinable();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("EXPORT")) {
        launch_export(session);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        // No incremental progress hook in the renderer — an honest
        // spinner until the result lands.
        const int dots = static_cast<int>(ImGui::GetTime() * 3.0) % 4;
        ImGui::TextColored(theme.text_dim, "rendering%.*s", dots, "...");
    } else if (!status_.empty()) {
        ImGui::TextColored(last_export_ok_ ? theme.text_dim : theme.text, "%s", status_.c_str());
    } else {
        ImGui::TextColored(theme.text_dim, "ready");
    }
    for (const std::string& file : last_files_) {
        ImGui::TextColored(theme.text_dim, "  %s", file.c_str());
    }
}

void ExportView::draw(app::ProjectSession& session, const Theme& theme, bool& open) {
    if (!open) {
        return;
    }
    poll_worker();
    prefill_defaults(session);
    ImGui::SetNextWindowPos(ImVec2(500, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("EXPORT", &open)) {
        const bool busy = worker_.joinable();
        ImGui::BeginDisabled(busy);
        draw_preset_row();
        draw_output_section(session);
        draw_format_section();
        draw_range_section(session);
        draw_stems_section(session);
        draw_post_section();
        draw_metadata_section();
        ImGui::EndDisabled();
        draw_status_section(session, theme);
    }
    ImGui::End();
}

} // namespace nt::ui
