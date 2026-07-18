// modplay/module_player — faithful tracker-module playback via
// libopenmpt (the design answer to the web app's import-only fidelity
// gap, Docs/Plan_NativePort/05-module-playback.md). A loaded module is
// an audio source the engine mixes like any other; the tracker's own
// sequencer is not involved.
//
// Threading: load()/unload() run on the UI thread while the player is
// detached from the engine (kSetModule(nullptr) first). render() runs
// on the audio thread; libopenmpt's render path is allocation-free
// after load. Position atomics flow audio→UI.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace openmpt {
class module;
}

namespace nt::modplay {

class ModulePlayer {
public:
    ModulePlayer();
    ~ModulePlayer();

    ModulePlayer(const ModulePlayer&) = delete;
    ModulePlayer& operator=(const ModulePlayer&) = delete;
    ModulePlayer(ModulePlayer&&) = delete;
    ModulePlayer& operator=(ModulePlayer&&) = delete;

    // Parses a module from memory. Returns false with `error()` set on
    // unsupported/corrupt data. Never call while attached to the engine.
    bool load(const std::uint8_t* data, std::size_t size);
    void unload();

    [[nodiscard]] bool loaded() const { return module_ != nullptr; }

    [[nodiscard]] const std::string& error() const { return error_; }

    // Metadata (valid while loaded).
    [[nodiscard]] const std::string& title() const { return title_; }

    [[nodiscard]] const std::string& format() const { return format_; }

    [[nodiscard]] double duration_seconds() const { return duration_; }

    // Audio thread: mixes `frames` stereo frames at `rate` ADDITIVELY
    // into `interleaved`. Returns false once the module has ended.
    bool render(float* interleaved, std::uint32_t frames, std::uint32_t rate);

    // UI thread: seek (safe while playing — libopenmpt serialises
    // internally against render via its own position state).
    void seek_seconds(double seconds);

    // Position published by the audio thread.
    [[nodiscard]] double position_seconds() const {
        return position_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int current_order() const { return order_.load(std::memory_order_relaxed); }

    [[nodiscard]] int current_row() const { return row_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<openmpt::module> module_;
    std::string error_;
    std::string title_;
    std::string format_;
    double duration_ = 0.0;

    std::atomic<double> position_{0.0};
    std::atomic<int> order_{0};
    std::atomic<int> row_{0};
};

} // namespace nt::modplay
