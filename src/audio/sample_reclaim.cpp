#include "audio/sample_reclaim.h"

#include <algorithm>

namespace nt::audio {

void SampleReclaimer::commit_staged(std::uint64_t retired_at_serial) {
    for (std::shared_ptr<void>& object : staged_) {
        entries_.push_back({std::move(object), retired_at_serial});
        ++retired_;
    }
    staged_.clear();
}

void SampleReclaimer::sweep(std::uint64_t observed_serial) {
    if (disarmed_) {
        return;
    }
    const auto freeable = [observed_serial](const Entry& entry) {
        return entry.retired_at <= observed_serial;
    };
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), freeable), entries_.end());
    freed_ += before - entries_.size();
}

} // namespace nt::audio
