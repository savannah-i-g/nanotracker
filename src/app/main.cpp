// Application entry point: settings → window → ImGui → theme → CRT
// pass → audio engine, then the frame loop. Owns the main menu bar,
// the dock layout (default DAW arrangement, first-run build, VIEW →
// reset) and the transport shell strip; tracker views live in ui/.
#include "api/local_api.h"
#include "app/autosave.h"
#include "app/input_script.h"
#include "app/midi_record.h"
#include "app/project_session.h"
#include "app/version.h"
#include "audio/audio_engine.h"
#include "engine/tracker_engine.h"
#include "io/export_render.h"
#include "io/settings.h"
#include "midi/midi_io.h"
#include "midi/midi_learn.h"
#include "midi/midi_out_thread.h"
#include "modplay/module_player.h"
#include "platform/app_window.h"
#include "platform/imgui_context.h"
#include "platform/paths.h"
#include "ui/crt_pass.h"
#include "ui/export_view.h"
#include "ui/fx_mixer_view.h"
#include "ui/instrument_table_view.h"
#include "ui/local_api_view.h"
#include "ui/midi_view.h"
#include "ui/module_view.h"
#include "ui/pattern_view.h"
#include "ui/piano_roll_view.h"
#include "ui/sample_browser_view.h"
#include "ui/sample_view.h"
#include "ui/theme.h"
#include "ui/workspace_view.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <glad/gl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <vector>

namespace {

// Automation hooks (verification runs, never user-facing):
//   --frames N   exit through the normal shutdown path after N frames
//   --tone       start with the test tone on
//   --play FILE  load FILE and play it once at startup
struct CliOptions {
    long frame_limit = 0;
    bool tone = false;
    bool play_demo = false;
    const char* play_path = nullptr;
    const char* load_path = nullptr;   // .ftrk to open at startup
    bool autoplay = false;             // start the transport after load
    const char* script_path = nullptr; // scripted-input verification run
    const char* module_path = nullptr; // module to load and play at startup
    bool fx_demo = false;              // add a bitcrusher FX channel after load
    bool workspace_demo = false;       // patch demo cables for verification runs
    bool workspace_dangle = false;     // reroute ch02 into a dangling node
    bool no_crt = false;               // scripted mouse runs need unwarped pixels
    const char* plugin_path = nullptr; // .ntins/.ntsfx to load at startup
    bool plugin_demo = false;          // spawn the loaded plugin + hold a note
    const char* clap_path = nullptr;   // .clap library to load at startup
    bool clap_demo = false;            // spawn its first plugin + hold a note
    bool clap_editor = false;          // also open the plugin's editor window
    const char* vst3_path = nullptr;   // .vst3 module to load at startup
    bool vst3_demo = false;            // spawn its first class + hold a note
    bool vst3_editor = false;          // also open the plugin's editor window
    bool seq_demo = false;             // add piano-roll notes to the loaded project
    const char* save_path = nullptr;   // write the project as .ftrk and continue
    const char* export_path = nullptr; // offline render (.wav/.ogg/.mp3) and exit
};

CliOptions parse_cli(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            options.frame_limit = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--tone") == 0) {
            options.tone = true;
        } else if (std::strcmp(argv[i], "--play-demo") == 0) {
            options.play_demo = true;
        } else if (std::strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
            options.play_path = argv[++i];
        } else if (std::strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
            options.load_path = argv[++i];
        } else if (std::strcmp(argv[i], "--autoplay") == 0) {
            options.autoplay = true;
        } else if (std::strcmp(argv[i], "--input-script") == 0 && i + 1 < argc) {
            options.script_path = argv[++i];
        } else if (std::strcmp(argv[i], "--play-module") == 0 && i + 1 < argc) {
            options.module_path = argv[++i];
        } else if (std::strcmp(argv[i], "--fx-demo") == 0) {
            options.fx_demo = true;
        } else if (std::strcmp(argv[i], "--workspace-demo") == 0) {
            options.workspace_demo = true;
        } else if (std::strcmp(argv[i], "--workspace-dangle") == 0) {
            options.workspace_dangle = true;
        } else if (std::strcmp(argv[i], "--no-crt") == 0) {
            options.no_crt = true;
        } else if (std::strcmp(argv[i], "--load-plugin") == 0 && i + 1 < argc) {
            options.plugin_path = argv[++i];
        } else if (std::strcmp(argv[i], "--plugin-demo") == 0) {
            options.plugin_demo = true;
        } else if (std::strcmp(argv[i], "--load-clap") == 0 && i + 1 < argc) {
            options.clap_path = argv[++i];
        } else if (std::strcmp(argv[i], "--clap-demo") == 0) {
            options.clap_demo = true;
        } else if (std::strcmp(argv[i], "--clap-editor") == 0) {
            options.clap_editor = true;
        } else if (std::strcmp(argv[i], "--load-vst3") == 0 && i + 1 < argc) {
            options.vst3_path = argv[++i];
        } else if (std::strcmp(argv[i], "--vst3-demo") == 0) {
            options.vst3_demo = true;
        } else if (std::strcmp(argv[i], "--vst3-editor") == 0) {
            options.vst3_editor = true;
        } else if (std::strcmp(argv[i], "--seq-demo") == 0) {
            options.seq_demo = true;
        } else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            options.save_path = argv[++i];
        } else if (std::strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
            options.export_path = argv[++i];
        }
    }
    return options;
}

// Writes the current framebuffer as binary PPM (P6). PPM needs no
// dependency and converts losslessly offline; used only by scripted
// verification runs.
void write_framebuffer_ppm(const char* path, int width, int height) {
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    std::vector<unsigned char> pixels(w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

    // Plain C stream I/O keeps the writer dependency-free.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "screenshot: cannot write %s\n", path);
        return;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    // GL rows are bottom-up; PPM is top-down.
    for (std::size_t y = h; y-- > 0;) {
        std::fwrite(pixels.data() + (y * w * 3), 1, w * 3, file);
    }
    std::fclose(file); // NOLINT(cppcoreguidelines-owning-memory)
}

// Owns every loaded sample for the process lifetime: entries are never
// removed while the engine runs, so raw pointers handed to the audio
// thread stay valid.
using SampleRegistry = std::vector<std::unique_ptr<nt::audio::SampleBuffer>>;

// Everything the --play-demo automation hook owns: a generated
// two-channel project (kick + square lead) and its playback bundle.
// Exercises the sequencer → voice → device pipeline without any file
// or UI involvement.
struct DemoSong {
    nt::engine::TrackerProject project;
    SampleRegistry samples;
    nt::audio::PlaybackBundle bundle;
};

std::unique_ptr<nt::audio::SampleBuffer>
make_demo_sample(std::uint32_t rate, float freq, float seconds, bool square, const char* name) {
    auto buffer = std::make_unique<nt::audio::SampleBuffer>();
    buffer->rate = rate;
    buffer->source_rate = rate;
    buffer->name = name;
    buffer->frames = static_cast<std::uint32_t>(static_cast<float>(rate) * seconds);
    buffer->interleaved.resize(static_cast<std::size_t>(buffer->frames) * 2);
    for (std::uint32_t i = 0; i < buffer->frames; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float phase = t * freq;
        float s = 0.0F;
        if (square) {
            s = phase - std::floor(phase) < 0.5F ? 0.6F : -0.6F;
        } else {
            s = std::sin(phase * 6.2831853F);
        }
        s *= std::exp(-3.0F * t); // decay envelope
        buffer->interleaved[static_cast<std::size_t>(i) * 2] = s;
        buffer->interleaved[(static_cast<std::size_t>(i) * 2) + 1] = s;
    }
    return buffer;
}

std::unique_ptr<DemoSong> build_demo_song(std::uint32_t rate) {
    auto demo = std::make_unique<DemoSong>();
    demo->project = nt::engine::create_project(2);
    auto& p = demo->project;
    p.name = "DEMO";

    // Sample 1: 60 Hz sine kick; sample 2: 440 Hz square lead. Both
    // authored at device rate with base_note 60, so tracker note 49
    // (C-4) plays them at native pitch.
    demo->samples.push_back(make_demo_sample(rate, 60.0F, 0.25F, false, "kick"));
    demo->samples.push_back(make_demo_sample(rate, 440.0F, 0.30F, true, "lead"));
    for (int id = 1; id <= 2; ++id) {
        nt::engine::TrackerSample meta;
        meta.id = id;
        meta.volume = 64;
        meta.pan = 128;
        meta.base_note = 60;
        p.samples.push_back(meta);
        demo->bundle.samples_by_id[static_cast<std::size_t>(id)] =
            demo->samples[static_cast<std::size_t>(id - 1)].get();
    }

    // Events every 8 rows across the 64-row pattern: kick on even
    // events, lead alternating C-4 / C-5.
    auto& rows = p.patterns[0].rows;
    for (std::size_t r = 0; r < 8; ++r) {
        if (r % 2 == 0) {
            rows[r * 8][0] = {.note = 49, .instrument = 1};
        }
        rows[r * 8][1] = {.note = static_cast<std::uint8_t>(r % 2 == 0 ? 49 : 61), .instrument = 2};
    }
    demo->bundle.project = &p;
    return demo;
}

void load_and_play(nt::audio::AudioEngine& audio, SampleRegistry& samples, const std::string& path,
                   std::string& status) {
    std::string error;
    auto sample = nt::audio::load_wav(path, audio.sample_rate(), error);
    if (sample == nullptr) {
        status = error;
        return;
    }
    status = sample->name + " (" + std::to_string(sample->frames) + " frames @ " +
             std::to_string(sample->rate) + ")";
    samples.push_back(std::move(sample));
    audio.send({.type = nt::audio::Command::Type::kPlaySample,
                .value = 0.0F,
                .sample = samples.back().get()});
}

struct ShellState {
    bool show_help = false;
    bool show_licences = false;
    bool show_export = false;
};

// Per-window visibility, driven by the VIEW menu. Defaults mirror the
// pre-menu surface: everything visible except the on-demand
// help/licences/export toggles (ShellState).
struct WindowVisibility {
    bool shell = true;
    bool pattern = true;
    bool piano_roll = true;
    bool workspace = true;
    bool fx_mixer = true;
    bool module = true;
    bool samples = true;
    bool instruments = true;
    bool sample_browser = true;
    bool midi = true;
    bool local_api = true;
    bool debug = false; // diagnostics surface, off by default
};

// The dock layout's top strip. Theme/CRT moved to the SETTINGS menu and
// project I/O to the FILE menu, so what remains reads like a DAW
// transport bar: transport controls, engine status, peak meters.
void draw_shell_window(nt::audio::AudioEngine& audio, nt::app::ProjectSession& session,
                       nt::app::Autosave& autosave) {
    ImGui::SetNextWindowSize(ImVec2(430, 140), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("nanoTracker shell")) {
        const nt::audio::EngineSnapshot& snap = audio.snapshot();

        // ── Transport row ────────────────────────────────────────────
        if (ImGui::Button(snap.transport_playing ? "STOP (F5)" : "PLAY (F5)")) {
            if (snap.transport_playing) {
                session.stop();
            } else {
                session.play();
            }
        }
        ImGui::SameLine();
        ImGui::Text("BPM %d  SPD %d", session.project().bpm, session.project().speed);
        if (snap.transport_playing) {
            ImGui::SameLine();
            ImGui::TextDisabled("ord %d  pat %d  row %02d", snap.order_pos, snap.pattern_index,
                                snap.row);
        }
        // App identity + frame rate, right-aligned on the transport row.
        std::array<char, 96> id_line{};
        std::snprintf(id_line.data(), id_line.size(), "%s %s  %.1f fps", nt::kAppName,
                      nt::kVersionString, static_cast<double>(ImGui::GetIO().Framerate));
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() -
                                                             ImGui::CalcTextSize(id_line.data()).x -
                                                             ImGui::GetStyle().WindowPadding.x));
        ImGui::TextDisabled("%s", id_line.data());

        // ── Engine status row + meters ───────────────────────────────
        if (!audio.running()) {
            ImGui::TextWrapped("audio device failed: %s", audio.device().error());
        } else {
            ImGui::TextDisabled("%s @ %u Hz", audio.device().backend_name(), snap.sample_rate);

            // Ballistic peak meters: instant attack, exponential release
            // (~2 s to the floor) — transients register at full height
            // and the fall reads at any frame rate.
            static float meter_l = 0.0F;
            static float meter_r = 0.0F;
            const float release = std::exp(-ImGui::GetIO().DeltaTime * 3.0F);
            meter_l = std::max(snap.peak_l, meter_l * release);
            meter_r = std::max(snap.peak_r, meter_r * release);
            ImGui::ProgressBar(meter_l, ImVec2(-1.0F, 4.0F), "");
            ImGui::ProgressBar(meter_r, ImVec2(-1.0F, 4.0F), "");
        }

        if (autosave.previous_run_crashed() && !autosave.latest_slot().empty()) {
            ImGui::TextWrapped("previous session did not exit cleanly");
            ImGui::SameLine();
            if (ImGui::SmallButton("recover autosave")) {
                if (!session.load_ftrk(autosave.latest_slot())) {
                    std::fprintf(stderr, "recover: %s\n", session.error().c_str());
                }
            }
        }
    }
    ImGui::End();
}

// Diagnostics surface (VIEW → DEBUG, off by default): the engine
// counters and the test-tone generator that used to crowd the transport
// shell. Floating — never docked into the default layout.
void draw_debug_window(nt::audio::AudioEngine& audio, bool& open) {
    if (!open) {
        return;
    }
    static bool tone_on = false;
    static float tone_freq = 440.0F;

    ImGui::SetNextWindowSize(ImVec2(430, 120), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("DEBUG", &open)) {
        const nt::audio::EngineSnapshot& snap = audio.snapshot();
        ImGui::TextDisabled("%s @ %u Hz, %llu pulls, %llu dropped cmds",
                            audio.device().backend_name(), snap.sample_rate,
                            static_cast<unsigned long long>(snap.pulls),
                            static_cast<unsigned long long>(audio.dropped_commands()));

        ImGui::BeginDisabled(!audio.running());
        if (ImGui::Checkbox("test tone", &tone_on)) {
            audio.send({.type = tone_on ? nt::audio::Command::Type::kToneOn
                                        : nt::audio::Command::Type::kToneOff,
                        .value = 0.0F,
                        .sample = nullptr});
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::SliderFloat("Hz", &tone_freq, 55.0F, 3520.0F, "%.0f",
                               ImGuiSliderFlags_Logarithmic)) {
            audio.send({.type = nt::audio::Command::Type::kToneFreq,
                        .value = tone_freq,
                        .sample = nullptr});
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

// One path-prompt modal (FILE → open… / save as…). Centred on appear;
// Enter in the field accepts. Returns true when confirmed with a
// non-empty path.
bool draw_path_dialog(const char* title, const char* verb, std::array<char, 512>& path) {
    bool accepted = false;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(420.0F);
        const bool entered =
            ImGui::InputTextWithHint("##path", "path/to/song.ftrk", path.data(), path.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(verb) || entered) {
            accepted = path[0] != '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return accepted;
}

// The main menu bar. Every action routes through the exact session/
// settings paths the shell window used — the menu is an additional
// surface, never new semantics. Returns the active theme (SETTINGS can
// switch it). `reset_layout` arms the dock rebuild consumed at the
// frame's dockspace point.
const nt::ui::Theme* draw_main_menu_bar(const nt::ui::Theme* active, nt::io::Settings& settings,
                                        nt::audio::AudioEngine& audio,
                                        nt::app::ProjectSession& session,
                                        nt::platform::AppWindow& window, ShellState& shell,
                                        WindowVisibility& vis, bool& reset_layout) {
    // Last-used project path, shared by open/save/save-as (previously
    // the shell window's path box).
    static std::array<char, 512> project_path{};
    bool want_open_dialog = false;
    bool want_save_dialog = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("FILE")) {
            if (ImGui::MenuItem("new project")) {
                session.new_project();
            }
            if (ImGui::MenuItem("open...")) {
                want_open_dialog = true;
            }
            ImGui::Separator();
            // "save" reuses the last path and falls through to the
            // dialog when none is set yet.
            if (ImGui::MenuItem("save")) {
                if (project_path[0] == '\0') {
                    want_save_dialog = true;
                } else if (!session.save_ftrk(project_path.data())) {
                    std::fprintf(stderr, "save: %s\n", session.error().c_str());
                }
            }
            if (ImGui::MenuItem("save as...")) {
                want_save_dialog = true;
            }
            if (ImGui::MenuItem("export...", nullptr, shell.show_export)) {
                shell.show_export = !shell.show_export;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("quit")) {
                // The platform close path: the frame loop exits through
                // window.should_close() and shutdown runs normally.
                glfwSetWindowShouldClose(window.native_handle(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("EDIT")) {
            nt::app::UndoStack& undo = session.undo();
            if (ImGui::MenuItem("undo", "Ctrl+Z", false, undo.can_undo())) {
                undo.undo();
            }
            if (ImGui::MenuItem("redo", "Ctrl+Y", false, undo.can_redo())) {
                undo.redo();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("VIEW")) {
            ImGui::MenuItem("nanoTracker shell", nullptr, &vis.shell);
            ImGui::MenuItem("PATTERN", nullptr, &vis.pattern);
            ImGui::MenuItem("piano roll", nullptr, &vis.piano_roll);
            ImGui::MenuItem("workspace", nullptr, &vis.workspace);
            ImGui::MenuItem("FX MIXER", nullptr, &vis.fx_mixer);
            ImGui::MenuItem("MODULE", nullptr, &vis.module);
            ImGui::MenuItem("SAMPLES", nullptr, &vis.samples);
            ImGui::MenuItem("INSTRUMENTS", nullptr, &vis.instruments);
            ImGui::MenuItem("SAMPLE BROWSER", nullptr, &vis.sample_browser);
            ImGui::MenuItem("midi", nullptr, &vis.midi);
            ImGui::MenuItem("LOCAL API", nullptr, &vis.local_api);
            ImGui::MenuItem("EXPORT", nullptr, &shell.show_export);
            ImGui::MenuItem("DEBUG", nullptr, &vis.debug);
            ImGui::Separator();
            if (ImGui::MenuItem("reset layout")) {
                reset_layout = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("SETTINGS")) {
            if (ImGui::BeginMenu("theme")) {
                for (const nt::ui::Theme& t : nt::ui::themes()) {
                    if (ImGui::MenuItem(t.name, nullptr, &t == active) && &t != active) {
                        active = &t;
                        settings.theme_id = t.id;
                        nt::ui::apply_theme(t);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("UI scale")) {
                // The drag edits a staged copy; the persisted factor
                // (which the frame-loop poll applies to metrics + font)
                // commits on RELEASE. Applying mid-drag rescales the
                // slider under the cursor and the remapped grab runs
                // away to the bound.
                static float staged_scale = settings.ui_scale;
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    staged_scale = settings.ui_scale; // follow external changes
                }
                ImGui::SetNextItemWidth(160.0F);
                ImGui::SliderFloat("##ui_scale", &staged_scale, 0.6F, 2.0F, "%.2fx",
                                   ImGuiSliderFlags_AlwaysClamp);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    settings.ui_scale = staged_scale;
                }
                if (ImGui::MenuItem("reset to 1.00x")) {
                    settings.ui_scale = 1.0F;
                    staged_scale = 1.0F;
                }
                ImGui::EndMenu();
            }
            // The CRT shader's bloom/scanline/vignette constants assume a
            // dark scene, so the pass is gated off on light themes
            // (crt.end) and its controls disabled here.
            ImGui::BeginDisabled(active->light);
            ImGui::MenuItem("CRT effect", nullptr, &settings.crt_enabled);
            ImGui::BeginDisabled(!settings.crt_enabled);
            ImGui::SetNextItemWidth(140.0F);
            ImGui::SliderFloat("intensity", &settings.crt_intensity, 0.0F, 1.0F, "%.2f");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            if (active->light) {
                ImGui::TextDisabled("CRT unavailable on light themes");
            }
            ImGui::Separator();
            if (audio.running()) {
                ImGui::TextDisabled("%s @ %u Hz", audio.device().backend_name(),
                                    audio.snapshot().sample_rate);
            } else {
                ImGui::TextDisabled("audio device unavailable");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("HELP")) {
            ImGui::MenuItem("help", nullptr, &shell.show_help);
            ImGui::MenuItem("licences", nullptr, &shell.show_licences);
            ImGui::Separator();
            std::array<char, 64> version_line{};
            std::snprintf(version_line.data(), version_line.size(), "%s %s", nt::kAppName,
                          nt::kVersionString);
            ImGui::MenuItem(version_line.data(), nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Popups open outside the menu scope so their ids resolve where
    // BeginPopupModal submits them (the classic OpenPopup-in-menu trap).
    if (want_open_dialog) {
        ImGui::OpenPopup("open project");
    }
    if (want_save_dialog) {
        ImGui::OpenPopup("save project as");
    }
    if (draw_path_dialog("open project", "open", project_path)) {
        if (!session.load_file(project_path.data())) {
            std::fprintf(stderr, "load: %s\n", session.error().c_str());
        }
    }
    if (draw_path_dialog("save project as", "save", project_path)) {
        if (!session.save_ftrk(project_path.data())) {
            std::fprintf(stderr, "save: %s\n", session.error().c_str());
        }
    }
    return active;
}

// The standard DAW arrangement, rebuilt into the viewport dockspace:
// transport strip across the top, pattern-dominant centre, sample
// management along the bottom, utility rail on the right — the web
// app's frame (menu/transport up top, grid centre, samples strip
// below). help/licences stay floating. Runs before the dockspace
// submits: on the first frame when no layout.ini existed, and on
// VIEW → reset layout. User rearrangements persist via layout.ini
// (platform/imgui_context).
void build_dock_layout(ImGuiID dockspace_id) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->WorkSize);

    // Each ratio is of the node being split: strip heights come off
    // first, then the rail takes 0.24 of what remains beside the
    // centre.
    ImGuiID center = dockspace_id;
    ImGuiID top = 0;
    ImGuiID bottom = 0;
    ImGuiID right = 0;
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.14F, &top, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26F, &bottom, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24F, &right, &center);

    // A lone transport strip needs no tab bar.
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(top)) {
        node->SetLocalFlags(node->LocalFlags | ImGuiDockNodeFlags_AutoHideTabBar);
    }

    ImGui::DockBuilderDockWindow("nanoTracker shell", top);
    ImGui::DockBuilderDockWindow("PATTERN", center);
    ImGui::DockBuilderDockWindow("piano roll", center);
    ImGui::DockBuilderDockWindow("workspace", center);
    ImGui::DockBuilderDockWindow("FX MIXER", center);
    ImGui::DockBuilderDockWindow("MODULE", center);
    ImGui::DockBuilderDockWindow("SAMPLES", bottom);
    ImGui::DockBuilderDockWindow("INSTRUMENTS", bottom);
    ImGui::DockBuilderDockWindow("SAMPLE BROWSER", bottom);
    ImGui::DockBuilderDockWindow("midi", right);
    ImGui::DockBuilderDockWindow("LOCAL API", right);
    ImGui::DockBuilderDockWindow("EXPORT", right);
    ImGui::DockBuilderFinish(dockspace_id);
}

void draw_help_window(ShellState& shell) {
    if (!shell.show_help) {
        return;
    }
    // Floating utility window: centred on each open, never part of the
    // default dock layout.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2{460, 380}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("help", &shell.show_help)) {
        ImGui::SeparatorText("pattern editor");
        ImGui::TextWrapped(
            "Arrows move the cursor; note keys enter notes (Z-M bottom octave, Q-P upper); "
            "digits edit instrument/volume/effect columns; Delete clears; Ctrl+Z undoes. "
            "Space toggles the transport.");
        ImGui::TextWrapped(
            "Blocks: Shift+arrows (or mouse drag) select; Ctrl+C/X/V copy/cut/paste; "
            "Delete clears the block; Ctrl+I interpolates volume or effect params; "
            "Alt+F1/F2 transpose a semitone, Alt+F3/F4 an octave. Right-click lists "
            "every block operation.");
        ImGui::SeparatorText("workspace");
        ImGui::TextWrapped(
            "Drag from an output jack to a compatible input to patch a cable. The midpoint "
            "chip toggles tap (fan-out) vs reroute (replaces the instrument's master route); "
            "right-click a cable to delete it. Feedback loops are legal — a cycle runs one "
            "block late.");
        ImGui::SeparatorText("plugins");
        ImGui::TextWrapped(
            "NTP archives (.ntins/.ntsfx) load from the workspace window, as do CLAP and "
            "VST3 libraries. External editors open as separate OS windows; every plugin "
            "also gets a generated parameter panel. MIDI-learn lives in the midi window.");
        ImGui::SeparatorText("sequence layers");
        ImGui::TextWrapped(
            "The piano roll layers up to four polyphonic lanes per channel on top of the "
            "pattern grid. Click adds a note, right-click removes. Drag over empty space "
            "rubber-band selects; Shift+click toggles; drag a note body to move the "
            "selection, its right edge to resize; Ctrl+C/V copy/paste. The toolbox row "
            "applies quantize, humanize, transpose, reverse, invert, arpeggiate, velocity "
            "curves and gate length to the selection (or all notes).");
    }
    ImGui::End();
}

void draw_licence_window(ShellState& shell) {
    if (!shell.show_licences) {
        return;
    }
    // Floating utility window, same contract as draw_help_window.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2{480, 340}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("licences", &shell.show_licences)) {
        ImGui::TextWrapped(
            "nanoTracker is free software: GNU General Public License v3 (see LICENSE). "
            "The NTP plugin headers (include/ntp) are MIT with a plugin exception, so "
            "plugin authors are never copyleft-bound.");
        ImGui::SeparatorText("third party");
        ImGui::TextWrapped(
            "GLFW (zlib), glad (MIT), Dear ImGui (MIT), OpenAL Soft (LGPL, dynamic), "
            "libopenmpt (BSD-3), dr_libs (PD/MIT), stb (PD/MIT), libsamplerate (BSD-2), "
            "miniz (MIT), CLAP (MIT), VST3 SDK (MIT), RtMidi (MIT-like), libogg/libvorbis "
            "(BSD), LAME via system library (LGPL, dynamic), nlohmann-json (MIT), Kode "
            "Mono (SIL OFL 1.1). Full texts: Docs/DEPENDENCIES.md and LICENSES/.");
    }
    ImGui::End();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const CliOptions cli = parse_cli(argc, argv);
        nt::io::Settings settings = nt::io::load_settings(nt::io::default_settings_path());
        if (cli.no_crt) {
            settings.crt_enabled = false;
        }

        nt::platform::AppWindow window({.title = "nanoTracker",
                                        .width = settings.window_width,
                                        .height = settings.window_height});
        nt::platform::ImGuiHost imgui(window,
                                      nt::platform::find_asset("fonts/KodeMono-Regular.ttf"));

        const nt::ui::Theme* theme = &nt::ui::theme_by_id(settings.theme_id);
        nt::ui::apply_theme(*theme);

        nt::ui::CrtPass crt;

        nt::audio::AudioEngine audio;
        SampleRegistry samples;
        if (!audio.start()) {
            std::fprintf(stderr, "audio: %s (continuing without sound)\n", audio.device().error());
        }

        if (cli.tone && audio.running()) {
            audio.send(
                {.type = nt::audio::Command::Type::kToneOn, .value = 0.0F, .sample = nullptr});
        }
        if (cli.play_path != nullptr && audio.running()) {
            std::string status;
            load_and_play(audio, samples, cli.play_path, status);
            std::fprintf(stderr, "play: %s\n", status.c_str());
        }
        nt::app::ProjectSession session(audio);
        nt::app::Autosave autosave(nt::platform::config_dir() / "autosave");
        ShellState shell;
        WindowVisibility vis;
        // Fixed dockspace id so the default layout can be built before
        // the dockspace first submits. First run (no layout.ini) builds
        // it; VIEW → reset layout re-arms the flag mid-session.
        const ImGuiID dockspace_id = ImHashStr("nanoTrackerDockspace");
        bool rebuild_dock_layout = !imgui.layout_file_existed();
        int dock_focus_countdown = 0;
        // Appearing windows steal focus on their first Begin (frame 1),
        // which would override the tab selections restored from
        // layout.ini at the next dock update. Clearing focus once after
        // the first frame's submissions lets the saved layout stand.
        bool clear_startup_focus = imgui.layout_file_existed();
        nt::ui::PatternView pattern_view;
        nt::ui::SampleView sample_view;
        nt::ui::SampleBrowserView sample_browser_view;
        nt::modplay::ModulePlayer module_player;
        nt::ui::ModuleView module_view;
        nt::ui::WorkspaceView workspace_view;
        nt::ui::PianoRollView piano_roll_view;
        nt::ui::ExportView export_view(nt::platform::config_dir() / "export_presets.json");
        nt::midi::MidiInput midi_input;
        nt::midi::MidiOutputPort midi_output;
        nt::midi::MidiOutThread midi_out_thread(audio, midi_output);
        // Ext MIDI In graph nodes drain this ring on the audio thread
        // (midi_input outlives the engine's render loop — both stop
        // before scope exit).
        audio.set_ext_midi_source(&midi_input.graph_ring());
        nt::midi::MidiLearn midi_learn(nt::platform::config_dir() / "midi_mappings.json");
        // Inbound-note modes (preview/enter/record); the pattern view
        // is the step-entry cursor surface.
        nt::app::MidiRecord midi_record(audio, pattern_view);
        nt::ui::MidiView midi_view(midi_input, midi_output, midi_out_thread, midi_learn,
                                   midi_record);
        // Local API server (constructed after the session: destruction
        // joins its threads before the session goes away). Server
        // threads only enqueue; requests execute in the frame loop.
        nt::api::LocalApiServer local_api;
        if (settings.local_api_enabled) {
            if (settings.local_api_token.empty()) {
                settings.local_api_token = nt::api::generate_token();
            }
            if (!local_api.start(settings.local_api_port, settings.local_api_token)) {
                std::fprintf(stderr, "local api: %s\n", local_api.error().c_str());
                settings.local_api_enabled = false;
            }
        }

        if (cli.module_path != nullptr && audio.running()) {
            if (module_view.load_file(audio, module_player, cli.module_path)) {
                audio.send({.type = nt::audio::Command::Type::kModulePlay});
            } else {
                std::fprintf(stderr, "module: %s\n", module_player.error().c_str());
            }
        }

        if (cli.load_path != nullptr) {
            if (!session.load_file(cli.load_path)) {
                std::fprintf(stderr, "load: %s\n", session.error().c_str());
            }
            for (const std::string& warning : session.load_warnings()) {
                std::fprintf(stderr, "load warning: %s\n", warning.c_str());
            }
        }
        if (cli.plugin_path != nullptr) {
            const std::string plugin_id = session.load_plugin_file(cli.plugin_path);
            if (plugin_id.empty()) {
                std::fprintf(stderr, "plugin: %s\n", session.error().c_str());
            } else if (cli.plugin_demo) {
                const std::string node_id = session.add_plugin_node(plugin_id);
                // Bind instrument slot 1 to the instance and hold A-4
                // so captures can measure the plugin's output.
                nt::engine::InstrumentTableEntry entry;
                entry.type = nt::engine::InstrumentSourceType::kPlugin;
                entry.plugin_id = plugin_id;
                entry.workspace_id = node_id;
                session.set_instrument_entry(1, entry);
                session.preview_plugin_note(1, 69, 1.0F);
            }
            for (const std::string& warning : session.load_warnings()) {
                std::fprintf(stderr, "plugin warning: %s\n", warning.c_str());
            }
        }
        if (cli.clap_path != nullptr) {
            if (!session.load_clap_file(cli.clap_path)) {
                std::fprintf(stderr, "clap: %s\n", session.error().c_str());
            } else if (cli.clap_demo) {
                const auto catalog = session.clap_catalog();
                if (!catalog.empty()) {
                    const std::string node_id =
                        session.add_clap_node(catalog.front().descriptor.id);
                    nt::engine::InstrumentTableEntry entry;
                    entry.type = nt::engine::InstrumentSourceType::kPlugin;
                    entry.plugin_id = "clap:" + catalog.front().descriptor.id;
                    entry.workspace_id = node_id;
                    session.set_instrument_entry(1, entry);
                    session.preview_plugin_note(1, 69, 1.0F);
                    if (cli.clap_editor && !session.open_clap_editor(node_id)) {
                        std::fprintf(stderr, "clap editor: %s\n", session.error().c_str());
                    }
                }
            }
        }
        if (cli.vst3_path != nullptr) {
            if (!session.load_vst3_file(cli.vst3_path)) {
                std::fprintf(stderr, "vst3: %s\n", session.error().c_str());
            } else if (cli.vst3_demo) {
                const auto catalog = session.vst3_catalog();
                if (!catalog.empty()) {
                    const std::string node_id =
                        session.add_vst3_node(catalog.front().descriptor.id);
                    nt::engine::InstrumentTableEntry entry;
                    entry.type = nt::engine::InstrumentSourceType::kPlugin;
                    entry.plugin_id = "vst3:" + catalog.front().descriptor.id;
                    entry.workspace_id = node_id;
                    session.set_instrument_entry(1, entry);
                    session.preview_plugin_note(1, 69, 1.0F);
                    if (cli.vst3_editor && !session.open_vst3_editor(node_id)) {
                        std::fprintf(stderr, "vst3 editor: %s\n", session.error().c_str());
                    }
                }
            }
        }
        if (cli.workspace_demo) {
            // Verification: one tap cable (ch01 → master, fans out
            // alongside the dry path) and one reroute (ch02 flows only
            // through the cable), plus a SUM node in the ch03 path.
            session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch01"},
                              {.node_id = nt::graph::kMasterInId, .port_id = "main"},
                              nt::graph::CableMode::kTap);
            session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch02"},
                              {.node_id = nt::graph::kMasterInId, .port_id = "main"},
                              nt::graph::CableMode::kReroute);
            const std::string sum_id = session.add_sum_node();
            session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch03"},
                              {.node_id = sum_id, .port_id = "in"}, nt::graph::CableMode::kTap);
            session.add_cable({.node_id = sum_id, .port_id = "out"},
                              {.node_id = nt::graph::kMasterInId, .port_id = "main"},
                              nt::graph::CableMode::kTap);
        }
        if (cli.workspace_dangle) {
            // Verification: reroute ch02 into a SUM whose output goes
            // nowhere — the channel must fall silent (dry suppressed,
            // cable path a dead end).
            const std::string sum_id = session.add_sum_node();
            session.add_cable({.node_id = nt::graph::kTrackerBusId, .port_id = "ch02"},
                              {.node_id = sum_id, .port_id = "in"}, nt::graph::CableMode::kReroute);
        }
        if (cli.fx_demo) {
            // Verification: a hard bitcrush (3 kHz hold) whose aliasing
            // images are spectrally checkable in captures.
            session.add_fx_channel();
            session.add_fx_module(0, "bitcrusher");
            session.set_fx_module_param(0, 0, 0, 16.0F);   // bits
            session.set_fx_module_param(0, 0, 1, 3000.0F); // hold rate
            session.set_fx_module_param(0, 0, 2, 100.0F);  // wet
            session.set_fx_module_param(0, 0, 3, 0.0F);    // dry
        }
        if (cli.seq_demo) {
            // Verification: an arpeggio on channel 0 layer 0 with
            // instrument 1 — octave-up pitches so the capture can
            // separate sequence voices from pattern channels.
            const int speed = session.project().speed;
            for (int i = 0; i < 8; ++i) {
                session.seq_add_note(0, 0, 0,
                                     {.pitch = 84,
                                      .start_tick = i * 4 * speed,
                                      .duration_ticks = speed * 2,
                                      .velocity = 110});
            }
            session.seq_set_layer_instrument(0, 0, 0, 1);
        }
        if (cli.export_path != nullptr) {
            const std::filesystem::path out_path = cli.export_path;
            nt::io::ExportFormat format = nt::io::ExportFormat::kWav;
            if (out_path.extension() == ".ogg") {
                format = nt::io::ExportFormat::kOgg;
            } else if (out_path.extension() == ".mp3") {
                format = nt::io::ExportFormat::kMp3;
            }
            const nt::io::ExportResult exported = session.export_current(out_path, format);
            if (exported.ok) {
                std::fprintf(stderr, "exported: %s (%.2fs)\n", cli.export_path, exported.seconds);
            } else {
                std::fprintf(stderr, "export failed: %s\n", exported.error.c_str());
            }
            audio.stop();
            return exported.ok ? 0 : 1;
        }
        if (cli.save_path != nullptr) {
            if (!session.save_ftrk(cli.save_path)) {
                std::fprintf(stderr, "save: %s\n", session.error().c_str());
            } else {
                std::fprintf(stderr, "saved: %s\n", cli.save_path);
            }
        }
        if (cli.autoplay) {
            session.play();
        }

        nt::app::InputScript script;
        if (cli.script_path != nullptr) {
            std::string script_error;
            if (!script.load(cli.script_path, script_error)) {
                std::fprintf(stderr, "input script: %s\n", script_error.c_str());
                return 1;
            }
            imgui.enable_scripted_mouse();
        }

        std::unique_ptr<DemoSong> demo;
        if (cli.play_demo && audio.running()) {
            demo = build_demo_song(audio.sample_rate());
            audio.send({.type = nt::audio::Command::Type::kSetBundle,
                        .value = 0.0F,
                        .sample = nullptr,
                        .bundle = &demo->bundle});
            audio.send({.type = nt::audio::Command::Type::kTransportPlay,
                        .value = 0.0F,
                        .sample = nullptr,
                        .bundle = nullptr});
        }

        long frame_count = 0;
        // The one styling entry point, run outside any frame: reset the
        // style metrics, restore the palette colors, set the font-scale
        // fields — in that order. 0 forces the first-frame build; the
        // triple reruns live on UI-scale edits and monitor-DPI changes.
        float applied_scale = 0.0F;
        while (!window.should_close() && !script.quit_requested() &&
               (cli.frame_limit == 0 || frame_count++ < cli.frame_limit)) {
            const float total_scale = window.content_scale() * settings.ui_scale;
            if (total_scale != applied_scale) {
                applied_scale = total_scale;
                nt::ui::apply_style_metrics(total_scale);
                nt::ui::apply_theme(*theme);
                nt::platform::ImGuiHost::apply_font_scale(window.content_scale(),
                                                          settings.ui_scale);
            }
            window.begin_frame(theme->background.x, theme->background.y, theme->background.z);
            if (script.active()) {
                script.update(); // queue scripted events before NewFrame
            }

            int fb_w = 0;
            int fb_h = 0;
            window.framebuffer_size(fb_w, fb_h);
            crt.begin(fb_w, fb_h, theme->background.x, theme->background.y, theme->background.z);

            nt::platform::ImGuiHost::begin_frame();
            // Menu first (it can arm a dock rebuild), then the rebuild,
            // then the dockspace submits and picks the nodes up.
            theme = draw_main_menu_bar(theme, settings, audio, session, window, shell, vis,
                                       rebuild_dock_layout);
            if (rebuild_dock_layout) {
                rebuild_dock_layout = false;
                build_dock_layout(dockspace_id);
                dock_focus_countdown = 4;
            }
            ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(),
                                         ImGuiDockNodeFlags_PassthruCentralNode);
            // Default front tabs after a rebuild. Tab selection tracks
            // g.NavWindow once per node update and appearing windows
            // steal focus on their first Begin, so the picks are staged
            // one per frame; PATTERN comes last and keeps keyboard
            // focus.
            if (dock_focus_countdown > 0) {
                --dock_focus_countdown;
                if (dock_focus_countdown == 2) {
                    ImGui::SetWindowFocus("midi");
                } else if (dock_focus_countdown == 1) {
                    ImGui::SetWindowFocus("SAMPLES");
                } else if (dock_focus_countdown == 0) {
                    ImGui::SetWindowFocus("PATTERN");
                }
            }
            if (vis.shell) {
                draw_shell_window(audio, session, autosave);
            }
            draw_help_window(shell);
            draw_licence_window(shell);
            if (vis.debug) {
                draw_debug_window(audio, vis.debug);
            }
            autosave.update(session, std::chrono::duration<double>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
            if (vis.pattern) {
                pattern_view.draw(session, audio.snapshot(), *theme);
            }
            if (vis.module) {
                module_view.draw(audio, module_player, *theme);
            }
            if (vis.samples) {
                sample_view.draw(session, *theme);
            }
            // Auditioned files join `samples` (process-lifetime, same
            // contract as load & play); loads target the slot selected
            // in the SAMPLES window.
            if (vis.sample_browser) {
                sample_browser_view.draw(session, audio, samples, sample_view.selected_slot(),
                                         *theme);
            }
            if (vis.instruments) {
                nt::ui::InstrumentTableView::draw(session, *theme);
            }
            if (vis.fx_mixer) {
                nt::ui::FxMixerView::draw(session, *theme);
            }
            if (vis.workspace) {
                workspace_view.draw(session, settings, *theme);
            }
            if (vis.piano_roll) {
                piano_roll_view.draw(session, *theme);
            }
            // The midi window's draw owns the device-ring drain
            // (ui/midi_view.h), so hiding it also pauses device MIDI
            // intake until re-shown; bus events (midi_record.update)
            // are unaffected.
            if (vis.midi) {
                midi_view.draw(session, *theme);
            }
            // Cabled master.midi.in events, drained after the MIDI
            // window's device drain: per frame, device notes apply
            // first, then bus notes (each source stays FIFO).
            midi_record.update(session);
            // Queued Local API requests execute here — the frame loop
            // is the session's single consumer (api/local_api.h).
            local_api.process_pending(session, audio);
            if (vis.local_api) {
                nt::ui::LocalApiView::draw(local_api, settings, *theme);
            }
            export_view.draw(session, *theme, shell.show_export);
            if (clear_startup_focus) {
                clear_startup_focus = false;
                ImGui::SetWindowFocus(nullptr);
            }
            session.update_clap_editors();
            session.update_vst3_editors();
            session.sweep_retired();
            nt::platform::ImGuiHost::end_frame();

            crt.end(settings.crt_enabled && !theme->light, settings.crt_intensity);
            if (std::string shot_path; script.take_screenshot(shot_path)) {
                write_framebuffer_ppm(shot_path.c_str(), fb_w, fb_h);
            }
            window.end_frame();
        }

        autosave.end_session();
        local_api.stop(); // joins server threads before teardown
        audio.stop();
        window.window_size(settings.window_width, settings.window_height);
        if (!nt::io::save_settings(settings, nt::io::default_settings_path())) {
            std::fprintf(stderr, "settings: save failed\n");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "nanotracker %s: fatal: %s\n", nt::kVersionString, error.what());
        return 1;
    }
}
