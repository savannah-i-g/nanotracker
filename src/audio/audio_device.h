// audio/audio_device — the pull-model device contract.
// A device owns an OS audio stream and pulls interleaved stereo float
// frames from the callback on its mixer thread. Everything above this
// interface is backend-agnostic; OpenAL specifics live entirely in
// audio_device_openal.cpp (see Docs/Plan_NativePort/03-audio-backend.md).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace nt::audio {

// Fills `interleaved` with exactly `frames` stereo frames. Invoked on
// the device's real-time thread: no allocation, locks, or blocking
// (enforced by rt/rt_assert in debug builds).
using RenderCallback = std::function<void(float* interleaved, std::uint32_t frames)>;

struct DeviceInfo {
    std::uint32_t sample_rate = 0; // actual rate, queried after open
    std::uint32_t refresh_hz = 0;  // device period-rate hint, 0 = unknown
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    AudioDevice() = default;
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    AudioDevice(AudioDevice&&) = delete;
    AudioDevice& operator=(AudioDevice&&) = delete;

    // Opens the stream at (a rate near) `requested_rate` and starts
    // pulling. The callback is retained until stop(). Returns false
    // with a reason via error() on failure.
    virtual bool start(std::uint32_t requested_rate, RenderCallback callback) = 0;
    virtual void stop() = 0;

    [[nodiscard]] virtual DeviceInfo info() const = 0;
    [[nodiscard]] virtual const char* backend_name() const = 0;
    [[nodiscard]] virtual const char* error() const = 0;
};

// OpenAL Soft backend via AL_SOFT_callback_buffer. The classic
// buffer-queue fallback is deliberately absent until the Windows
// validation pass demands it (recorded in Docs/PROGRESS.md).
std::unique_ptr<AudioDevice> make_openal_device();

} // namespace nt::audio
