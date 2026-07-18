#include "modplay/module_player.h"

#include <array>
#include <libopenmpt/libopenmpt.hpp>
#include <vector>

namespace nt::modplay {

ModulePlayer::ModulePlayer() = default;

ModulePlayer::~ModulePlayer() = default;

bool ModulePlayer::load(const std::uint8_t* data, std::size_t size) {
    try {
        auto module = std::make_unique<openmpt::module>(data, size);
        title_ = module->get_metadata("title");
        format_ = module->get_metadata("type_long");
        duration_ = module->get_duration_seconds();
        // Stereo separation matching classic hardware mixing rather
        // than the harsh 100% default.
        module->set_render_param(openmpt::module::RENDER_STEREOSEPARATION_PERCENT, 60);
        module_ = std::move(module);
        error_.clear();
        position_.store(0.0, std::memory_order_relaxed);
        return true;
    } catch (const openmpt::exception& e) {
        error_ = e.what();
        module_.reset();
        return false;
    }
}

void ModulePlayer::unload() {
    module_.reset();
    title_.clear();
    format_.clear();
    duration_ = 0.0;
}

bool ModulePlayer::render(float* interleaved, std::uint32_t frames, std::uint32_t rate) {
    if (module_ == nullptr) {
        return false;
    }
    // libopenmpt writes into its own buffer; mix additively into the
    // engine block. The scratch is fixed-capacity: the engine's block
    // size bounds `frames`.
    std::array<float, 512> scratch{};
    std::uint32_t done = 0;
    bool alive = true;
    while (done < frames) {
        const std::uint32_t chunk = std::min<std::uint32_t>(frames - done, 256);
        const std::size_t got = module_->read_interleaved_stereo(static_cast<std::int32_t>(rate),
                                                                 chunk, scratch.data());
        for (std::size_t i = 0; i < got * 2; ++i) {
            interleaved[(static_cast<std::size_t>(done) * 2) + i] += scratch[i];
        }
        done += static_cast<std::uint32_t>(got);
        if (got < chunk) {
            alive = false; // module ended
            break;
        }
    }
    position_.store(module_->get_position_seconds(), std::memory_order_relaxed);
    order_.store(module_->get_current_order(), std::memory_order_relaxed);
    row_.store(module_->get_current_row(), std::memory_order_relaxed);
    return alive;
}

void ModulePlayer::seek_seconds(double seconds) {
    if (module_ != nullptr) {
        module_->set_position_seconds(seconds);
    }
}

} // namespace nt::modplay
