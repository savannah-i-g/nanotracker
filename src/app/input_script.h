// app/input_script — scripted UI input for automated verification.
// Actions are injected through ImGui's own input queue and screenshots
// are written by the app itself, so a verification run never touches
// the window manager, the real cursor, or keyboard focus of other
// applications. This is the only sanctioned UI-automation mechanism.
//
// Script grammar (one command per line, '#' starts a comment):
//   wait N          idle N frames
//   key SPEC        press+release a key; SPEC examples:
//                   q  5  F5  space  enter  tab  up  down  left  right
//                   delete  home  end  pageup  pagedown  equal
//                   with modifiers: ctrl+z  shift+tab
//   mouse X Y       move the cursor to framebuffer coordinates
//   mousedown [BTN] press a button (left default, or right)
//   mouseup [BTN]   release it; drags are mouse/mousedown/mouse/mouseup
//                   sequences interleaved with wait
//   shot PATH       write the framebuffer to PATH as binary PPM (P6)
//   quit            end the run through the normal shutdown path
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::app {

class InputScript {
public:
    // Loads and parses the script; returns false with a message on
    // malformed input (unknown command or key name).
    bool load(const std::filesystem::path& path, std::string& error);

    [[nodiscard]] bool active() const { return !actions_.empty(); }

    // Call once per frame BEFORE ImGui's NewFrame so injected events
    // land in that frame's input queue.
    void update();

    // True once per `shot` action, after which the path is consumed.
    // The caller captures the framebuffer at end-of-frame.
    bool take_screenshot(std::string& path);

    [[nodiscard]] bool quit_requested() const { return quit_; }

private:
    struct Action {
        enum class Kind : std::uint8_t {
            kWait,
            kKeyDown,
            kKeyUp,
            kMouseMove,
            kMouseDown,
            kMouseUp,
            kShot,
            kQuit,
        };
        Kind kind = Kind::kWait;
        int frames = 0; // kWait
        int key = 0;    // ImGuiKey for kKeyDown/kKeyUp
        bool ctrl = false;
        bool shift = false;
        float x = 0.0F; // kMouseMove
        float y = 0.0F;
        int button = 0;   // 0 left / 1 right for kMouseDown/kMouseUp
        std::string path; // kShot
    };

    std::vector<Action> actions_;
    std::size_t next_ = 0;
    int wait_counter_ = 0;
    std::string pending_shot_;
    bool quit_ = false;
};

} // namespace nt::app
