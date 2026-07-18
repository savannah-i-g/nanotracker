#include "app/autosave.h"

#include <fstream>

namespace nt::app {

Autosave::Autosave(std::filesystem::path dir)
    : dir_(std::move(dir)), lock_path_(dir_ / "session.lock") {
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    previous_crashed_ = std::filesystem::exists(lock_path_, ec);
    std::ofstream lock(lock_path_);
    lock << "nanotracker session in progress\n";
}

std::filesystem::path Autosave::latest_slot() const {
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    std::error_code ec;
    for (int slot = 0; slot < kSlots; ++slot) {
        const std::filesystem::path candidate = dir_ / ("slot" + std::to_string(slot) + ".ftrk");
        if (!std::filesystem::exists(candidate, ec)) {
            continue;
        }
        const auto time = std::filesystem::last_write_time(candidate, ec);
        if (newest.empty() || time > newest_time) {
            newest = candidate;
            newest_time = time;
        }
    }
    return newest;
}

void Autosave::update(ProjectSession& session, double now_seconds) {
    if (last_save_ == 0.0) {
        last_save_ = now_seconds; // first interval measured from launch
        return;
    }
    if (now_seconds - last_save_ < kIntervalSeconds) {
        return;
    }
    last_save_ = now_seconds;
    const std::filesystem::path slot = dir_ / ("slot" + std::to_string(next_slot_) + ".ftrk");
    next_slot_ = (next_slot_ + 1) % kSlots;
    session.save_ftrk(slot); // failure is non-fatal; next interval retries
}

void Autosave::end_session() {
    std::error_code ec;
    std::filesystem::remove(lock_path_, ec);
}

} // namespace nt::app
