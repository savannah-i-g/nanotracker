#include "audio/audio_device.h"
#include "rt/rt_assert.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <array>
#include <string>
#include <utility>

namespace nt::audio {

namespace {

class OpenAlDevice final : public AudioDevice {
public:
    OpenAlDevice() = default;
    ~OpenAlDevice() override { stop(); }

    OpenAlDevice(const OpenAlDevice&) = delete;
    OpenAlDevice& operator=(const OpenAlDevice&) = delete;
    OpenAlDevice(OpenAlDevice&&) = delete;
    OpenAlDevice& operator=(OpenAlDevice&&) = delete;

    bool start(std::uint32_t requested_rate, RenderCallback callback) override {
        if (device_ != nullptr) {
            return fail("already started");
        }

        device_ = alcOpenDevice(nullptr);
        if (device_ == nullptr) {
            return fail("no OpenAL output device");
        }

        const std::array<ALCint, 3> attrs = {ALC_FREQUENCY, static_cast<ALCint>(requested_rate),
                                             0};
        context_ = alcCreateContext(device_, attrs.data());
        if (context_ == nullptr || alcMakeContextCurrent(context_) == ALC_FALSE) {
            teardown();
            return fail("OpenAL context creation failed");
        }

        if (alIsExtensionPresent("AL_SOFT_callback_buffer") == AL_FALSE) {
            teardown();
            return fail("AL_SOFT_callback_buffer unavailable (OpenAL Soft required)");
        }
        if (alIsExtensionPresent("AL_EXT_FLOAT32") == AL_FALSE) {
            teardown();
            return fail("AL_EXT_FLOAT32 unavailable");
        }
        // Extension entry points arrive as untyped function pointers;
        // the cast is the API contract.
        buffer_callback_ = reinterpret_cast<LPALBUFFERCALLBACKSOFT>( // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            alGetProcAddress("alBufferCallbackSOFT"));
        if (buffer_callback_ == nullptr) {
            teardown();
            return fail("alBufferCallbackSOFT missing");
        }

        // The requested rate is a hint; the graph must run at whatever
        // the device actually opened with.
        ALCint actual_rate = 0;
        alcGetIntegerv(device_, ALC_FREQUENCY, 1, &actual_rate);
        ALCint refresh = 0;
        alcGetIntegerv(device_, ALC_REFRESH, 1, &refresh);
        info_.sample_rate = static_cast<std::uint32_t>(actual_rate);
        info_.refresh_hz = refresh > 0 ? static_cast<std::uint32_t>(refresh) : 0;

        callback_ = std::move(callback);

        alGenBuffers(1, &buffer_);
        alGenSources(1, &source_);
        buffer_callback_(buffer_, AL_FORMAT_STEREO_FLOAT32, actual_rate, &OpenAlDevice::pull, this);
        alSourcei(source_, AL_BUFFER, static_cast<ALint>(buffer_));
        alSourcePlay(source_);

        if (alGetError() != AL_NO_ERROR) {
            stop();
            return fail("OpenAL source setup failed");
        }
        return true;
    }

    void stop() override {
        if (device_ == nullptr) {
            return;
        }
        alSourceStop(source_);
        alSourcei(source_, AL_BUFFER, 0);
        alDeleteSources(1, &source_);
        alDeleteBuffers(1, &buffer_);
        teardown();
        callback_ = nullptr;
    }

    [[nodiscard]] DeviceInfo info() const override { return info_; }

    [[nodiscard]] const char* backend_name() const override {
        return "OpenAL Soft (callback buffer)";
    }

    [[nodiscard]] const char* error() const override { return error_.c_str(); }

private:
    bool fail(const char* message) {
        error_ = message;
        return false;
    }

    void teardown() {
        alcMakeContextCurrent(nullptr);
        if (context_ != nullptr) {
            alcDestroyContext(context_);
            context_ = nullptr;
        }
        if (device_ != nullptr) {
            alcCloseDevice(device_);
            device_ = nullptr;
        }
    }

    // Mixer-thread entry: OpenAL Soft pulls sample data on demand.
    // Must fill the full request — a short return stops the source.
    static ALsizei AL_APIENTRY pull(void* user, void* sampledata, ALsizei numbytes) {
        auto* self = static_cast<OpenAlDevice*>(user);
        const rt::RtScope rt_scope;

        constexpr ALsizei kFrameBytes = 2 * sizeof(float);
        const auto frames = static_cast<std::uint32_t>(numbytes / kFrameBytes);
        self->callback_(static_cast<float*>(sampledata), frames);
        return static_cast<ALsizei>(frames) * kFrameBytes;
    }

    ALCdevice* device_ = nullptr;
    ALCcontext* context_ = nullptr;
    ALuint buffer_ = 0;
    ALuint source_ = 0;
    LPALBUFFERCALLBACKSOFT buffer_callback_ = nullptr;
    RenderCallback callback_;
    DeviceInfo info_;
    std::string error_;
};

} // namespace

std::unique_ptr<AudioDevice> make_openal_device() {
    return std::make_unique<OpenAlDevice>();
}

} // namespace nt::audio
