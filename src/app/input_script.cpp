#include "app/input_script.h"

#include <fstream>
#include <imgui.h>
#include <sstream>
#include <unordered_map>

namespace nt::app {

namespace {

int key_from_name(const std::string& name) {
    static const std::unordered_map<std::string, int> kNamed = {
        {"space", ImGuiKey_Space},
        {"enter", ImGuiKey_Enter},
        {"tab", ImGuiKey_Tab},
        {"up", ImGuiKey_UpArrow},
        {"down", ImGuiKey_DownArrow},
        {"left", ImGuiKey_LeftArrow},
        {"right", ImGuiKey_RightArrow},
        {"delete", ImGuiKey_Delete},
        {"backspace", ImGuiKey_Backspace},
        {"home", ImGuiKey_Home},
        {"end", ImGuiKey_End},
        {"pageup", ImGuiKey_PageUp},
        {"pagedown", ImGuiKey_PageDown},
        {"equal", ImGuiKey_Equal},
        {"escape", ImGuiKey_Escape},
    };
    if (const auto it = kNamed.find(name); it != kNamed.end()) {
        return it->second;
    }
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') {
            return ImGuiKey_A + (c - 'a');
        }
        if (c >= '0' && c <= '9') {
            return ImGuiKey_0 + (c - '0');
        }
    }
    if (name.size() >= 2 && name[0] == 'F') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) {
            return ImGuiKey_F1 + (n - 1);
        }
    }
    return ImGuiKey_None;
}

} // namespace

bool InputScript::load(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot open input script " + path.string();
        return false;
    }

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream tokens(line);
        std::string command;
        if (!(tokens >> command)) {
            continue;
        }

        if (command == "wait") {
            Action a;
            a.kind = Action::Kind::kWait;
            tokens >> a.frames;
            actions_.push_back(a);
        } else if (command == "key") {
            std::string spec;
            tokens >> spec;
            Action a;
            a.kind = Action::Kind::kKeyDown;
            std::size_t start = 0;
            for (std::size_t plus = spec.find('+'); plus != std::string::npos;
                 plus = spec.find('+', start)) {
                const std::string mod = spec.substr(start, plus - start);
                if (mod == "ctrl") {
                    a.ctrl = true;
                } else if (mod == "shift") {
                    a.shift = true;
                } else {
                    error = "line " + std::to_string(line_no) + ": unknown modifier " + mod;
                    return false;
                }
                start = plus + 1;
            }
            a.key = key_from_name(spec.substr(start));
            if (a.key == ImGuiKey_None) {
                error = "line " + std::to_string(line_no) + ": unknown key " + spec;
                return false;
            }
            actions_.push_back(a);
            // Release on the following frame so IsKeyPressed sees one
            // clean press.
            Action up = a;
            up.kind = Action::Kind::kKeyUp;
            actions_.push_back(up);
        } else if (command == "mouse") {
            Action a;
            a.kind = Action::Kind::kMouseMove;
            if (!(tokens >> a.x >> a.y)) {
                error = "line " + std::to_string(line_no) + ": mouse needs X Y";
                return false;
            }
            actions_.push_back(a);
        } else if (command == "mousedown" || command == "mouseup") {
            Action a;
            a.kind = command == "mousedown" ? Action::Kind::kMouseDown : Action::Kind::kMouseUp;
            std::string btn;
            if (tokens >> btn) {
                if (btn == "right") {
                    a.button = 1;
                } else if (btn != "left") {
                    error = "line " + std::to_string(line_no) + ": unknown button " + btn;
                    return false;
                }
            }
            actions_.push_back(a);
        } else if (command == "shot") {
            Action a;
            a.kind = Action::Kind::kShot;
            tokens >> a.path;
            actions_.push_back(std::move(a));
        } else if (command == "quit") {
            Action a;
            a.kind = Action::Kind::kQuit;
            actions_.push_back(a);
        } else {
            error = "line " + std::to_string(line_no) + ": unknown command " + command;
            return false;
        }
    }
    return true;
}

void InputScript::update() {
    if (wait_counter_ > 0) {
        --wait_counter_;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();

    while (next_ < actions_.size()) {
        const Action& action = actions_[next_];
        switch (action.kind) {
        case Action::Kind::kWait:
            ++next_;
            wait_counter_ = action.frames;
            return;
        case Action::Kind::kKeyDown:
            if (action.ctrl) {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
            }
            if (action.shift) {
                io.AddKeyEvent(ImGuiMod_Shift, true);
            }
            io.AddKeyEvent(static_cast<ImGuiKey>(action.key), true);
            ++next_;
            return; // one event group per frame
        case Action::Kind::kKeyUp:
            io.AddKeyEvent(static_cast<ImGuiKey>(action.key), false);
            if (action.ctrl) {
                io.AddKeyEvent(ImGuiMod_Ctrl, false);
            }
            if (action.shift) {
                io.AddKeyEvent(ImGuiMod_Shift, false);
            }
            ++next_;
            return;
        case Action::Kind::kMouseMove:
            io.AddMousePosEvent(action.x, action.y);
            ++next_;
            return;
        case Action::Kind::kMouseDown:
            io.AddMouseButtonEvent(action.button, true);
            ++next_;
            return;
        case Action::Kind::kMouseUp:
            io.AddMouseButtonEvent(action.button, false);
            ++next_;
            return;
        case Action::Kind::kShot:
            pending_shot_ = action.path;
            ++next_;
            return;
        case Action::Kind::kQuit:
            quit_ = true;
            ++next_;
            return;
        }
    }
}

bool InputScript::take_screenshot(std::string& path) {
    if (pending_shot_.empty()) {
        return false;
    }
    path = pending_shot_;
    pending_shot_.clear();
    return true;
}

} // namespace nt::app
