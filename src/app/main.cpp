// Application entry point: settings → window → ImGui → theme → CRT
// pass → audio engine, then the frame loop. The shell UI here is the
// Stage 1/2 surface; tracker views replace it as they land.
#include "app/autosave.h"
#include "app/input_script.h"
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
#include "ui/midi_view.h"
#include "ui/module_view.h"
#include "ui/pattern_view.h"
#include "ui/piano_roll_view.h"
#include "ui/sample_browser_view.h"
#include "ui/sample_view.h"
#include "ui/theme.h"
#include "ui/workspace_view.h"

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

void draw_audio_section(nt::audio::AudioEngine& audio, SampleRegistry& samples) {
    static std::array<char, 512> path_buf{};
    static std::string load_status;
    static bool tone_on = false;
    static float tone_freq = 440.0F;

    ImGui::SeparatorText("audio");

    if (!audio.running()) {
        ImGui::TextWrapped("audio device failed: %s", audio.device().error());
        return;
    }

    const nt::audio::EngineSnapshot& snap = audio.snapshot();
    ImGui::TextDisabled("%s @ %u Hz, %llu pulls, %llu dropped cmds", audio.device().backend_name(),
                        snap.sample_rate, static_cast<unsigned long long>(snap.pulls),
                        static_cast<unsigned long long>(audio.dropped_commands()));

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
        audio.send(
            {.type = nt::audio::Command::Type::kToneFreq, .value = tone_freq, .sample = nullptr});
    }

    ImGui::InputTextWithHint("##wav", "path/to/sample.wav", path_buf.data(), path_buf.size());
    ImGui::SameLine();
    if (ImGui::Button("load & play")) {
        load_and_play(audio, samples, path_buf.data(), load_status);
    }
    if (!load_status.empty()) {
        ImGui::TextDisabled("%s", load_status.c_str());
    }

    if (snap.transport_playing) {
        ImGui::TextDisabled("transport: order %d pattern %d row %02d tick %d  %d bpm spd %d",
                            snap.order_pos, snap.pattern_index, snap.row, snap.tick, snap.bpm,
                            snap.speed);
    }

    // Ballistic peak meters: instant attack, exponential release
    // (~2 s to the floor) — transients register at full height and
    // the fall reads at any frame rate.
    static float meter_l = 0.0F;
    static float meter_r = 0.0F;
    const float release = std::exp(-ImGui::GetIO().DeltaTime * 3.0F);
    meter_l = std::max(snap.peak_l, meter_l * release);
    meter_r = std::max(snap.peak_r, meter_r * release);
    ImGui::ProgressBar(meter_l, ImVec2(-1.0F, 4.0F), "");
    ImGui::ProgressBar(meter_r, ImVec2(-1.0F, 4.0F), "");
}

struct ShellState {
    bool show_help = false;
    bool show_licences = false;
    bool show_export = false;
};

const nt::ui::Theme* draw_shell_window(const nt::ui::Theme* active, nt::io::Settings& settings,
                                       nt::audio::AudioEngine& audio, SampleRegistry& samples,
                                       nt::app::ProjectSession& session,
                                       nt::app::Autosave& autosave, ShellState& shell) {
    ImGui::SetNextWindowSize(ImVec2(430, 330), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("nanoTracker shell")) {
        ImGui::TextDisabled("%s %s — platform shell", nt::kAppName, nt::kVersionString);
        ImGui::Separator();

        if (ImGui::BeginCombo("theme", active->name)) {
            for (const nt::ui::Theme& t : nt::ui::themes()) {
                const bool selected = (&t == active);
                if (ImGui::Selectable(t.name, selected)) {
                    active = &t;
                    settings.theme_id = t.id;
                    nt::ui::apply_theme(t);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Checkbox("CRT effect", &settings.crt_enabled);
        ImGui::BeginDisabled(!settings.crt_enabled);
        ImGui::SliderFloat("intensity", &settings.crt_intensity, 0.0F, 1.0F, "%.2f");
        ImGui::EndDisabled();

        draw_audio_section(audio, samples);

        // ── Project I/O ──────────────────────────────────────────────
        ImGui::SeparatorText("project");
        if (autosave.previous_run_crashed() && !autosave.latest_slot().empty()) {
            ImGui::TextWrapped("previous session did not exit cleanly");
            ImGui::SameLine();
            if (ImGui::SmallButton("recover autosave")) {
                if (!session.load_ftrk(autosave.latest_slot())) {
                    std::fprintf(stderr, "recover: %s\n", session.error().c_str());
                }
            }
        }
        static std::array<char, 512> project_path{};
        ImGui::SetNextItemWidth(220.0F);
        ImGui::InputTextWithHint("##proj", "path/to/song.ftrk", project_path.data(),
                                 project_path.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("save")) {
            if (!session.save_ftrk(project_path.data())) {
                std::fprintf(stderr, "save: %s\n", session.error().c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("load")) {
            if (!session.load_file(project_path.data())) {
                std::fprintf(stderr, "load: %s\n", session.error().c_str());
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("export")) {
            shell.show_export = !shell.show_export;
        }

        ImGui::Separator();
        if (ImGui::SmallButton("help")) {
            shell.show_help = !shell.show_help;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("licences")) {
            shell.show_licences = !shell.show_licences;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f fps", static_cast<double>(ImGui::GetIO().Framerate));
    }
    ImGui::End();
    return active;
}

void draw_help_window(ShellState& shell) {
    if (!shell.show_help) {
        return;
    }
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
        nt::midi::MidiLearn midi_learn(nt::platform::config_dir() / "midi_mappings.json");
        nt::ui::MidiView midi_view(midi_input, midi_output, midi_out_thread, midi_learn);

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
        while (!window.should_close() && !script.quit_requested() &&
               (cli.frame_limit == 0 || frame_count++ < cli.frame_limit)) {
            window.begin_frame(theme->background.x, theme->background.y, theme->background.z);
            if (script.active()) {
                script.update(); // queue scripted events before NewFrame
            }

            int fb_w = 0;
            int fb_h = 0;
            window.framebuffer_size(fb_w, fb_h);
            crt.begin(fb_w, fb_h, theme->background.x, theme->background.y, theme->background.z);

            imgui.begin_frame();
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                         ImGuiDockNodeFlags_PassthruCentralNode);
            theme = draw_shell_window(theme, settings, audio, samples, session, autosave, shell);
            draw_help_window(shell);
            draw_licence_window(shell);
            autosave.update(session, std::chrono::duration<double>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
            pattern_view.draw(session, audio.snapshot(), *theme);
            module_view.draw(audio, module_player, *theme);
            sample_view.draw(session, *theme);
            // Auditioned files join `samples` (process-lifetime, same
            // contract as load & play); loads target the slot selected
            // in the SAMPLES window.
            sample_browser_view.draw(session, audio, samples, sample_view.selected_slot(), *theme);
            nt::ui::InstrumentTableView::draw(session, *theme);
            nt::ui::FxMixerView::draw(session, *theme);
            workspace_view.draw(session, settings, *theme);
            piano_roll_view.draw(session, *theme);
            midi_view.draw(session, *theme);
            export_view.draw(session, *theme, shell.show_export);
            session.update_clap_editors();
            session.sweep_retired();
            nt::platform::ImGuiHost::end_frame();

            crt.end(settings.crt_enabled, settings.crt_intensity);
            if (std::string shot_path; script.take_screenshot(shot_path)) {
                write_framebuffer_ppm(shot_path.c_str(), fb_w, fb_h);
            }
            window.end_frame();
        }

        autosave.end_session();
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
