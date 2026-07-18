#ifndef _WIN32

#include "ext/vst3_run_loop.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <poll.h>

// The VST3 COM seam is built on TUID char arrays (FUID converts to one
// for every iidEqual call); the decay warnings are the API's shape,
// not ours — same treatment as the CLAP/Xlib C-ABI files.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

namespace nt::ext {

namespace {

using namespace Steinberg;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Vst3RunLoop& Vst3RunLoop::instance() {
    static Vst3RunLoop loop;
    return loop;
}

tresult PLUGIN_API Vst3RunLoop::queryInterface(const TUID interface_id, void** obj) {
    if (obj == nullptr) {
        return kInvalidArgument;
    }
    if (FUnknownPrivate::iidEqual(interface_id, Linux::IRunLoop::iid) ||
        FUnknownPrivate::iidEqual(interface_id, FUnknown::iid)) {
        *obj = static_cast<Linux::IRunLoop*>(this);
        addRef();
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

tresult PLUGIN_API Vst3RunLoop::registerEventHandler(Linux::IEventHandler* handler,
                                                     Linux::FileDescriptor fd) {
    if (handler == nullptr || fd < 0) {
        return kInvalidArgument;
    }
    handler->addRef();
    // Appended entries are indexed past the dispatch snapshot, so a
    // registration made from inside a callback first fires next frame.
    fds_.push_back({.handler = handler, .fd = fd, .alive = true});
    return kResultOk;
}

tresult PLUGIN_API Vst3RunLoop::unregisterEventHandler(Linux::IEventHandler* handler) {
    if (handler == nullptr) {
        return kInvalidArgument;
    }
    // Tombstone only — entries vanish in compact() after the dispatch
    // pass, so unregistering from inside a callback never invalidates
    // the iteration (the Cockos-class host bug this guards against).
    bool found = false;
    for (FdEntry& entry : fds_) {
        if (entry.handler == handler && entry.alive) {
            entry.alive = false;
            found = true;
        }
    }
    if (!dispatching_) {
        compact();
    }
    return found ? kResultOk : kResultFalse;
}

tresult PLUGIN_API Vst3RunLoop::registerTimer(Linux::ITimerHandler* handler,
                                              Linux::TimerInterval milliseconds) {
    if (handler == nullptr) {
        return kInvalidArgument;
    }
    handler->addRef();
    // Sub-frame intervals degrade to once per dispatch — the frame
    // loop is the resolution floor, same as the CLAP timer pump.
    const double interval =
        static_cast<double>(std::max<Linux::TimerInterval>(1, milliseconds)) / 1000.0;
    timers_.push_back({.handler = handler,
                       .interval_s = interval,
                       .due = now_seconds() + interval,
                       .alive = true});
    return kResultOk;
}

tresult PLUGIN_API Vst3RunLoop::unregisterTimer(Linux::ITimerHandler* handler) {
    if (handler == nullptr) {
        return kInvalidArgument;
    }
    bool found = false;
    for (TimerEntry& entry : timers_) {
        if (entry.handler == handler && entry.alive) {
            entry.alive = false;
            found = true;
        }
    }
    if (!dispatching_) {
        compact();
    }
    return found ? kResultOk : kResultFalse;
}

void Vst3RunLoop::dispatch() {
    if (dispatching_) {
        return; // re-entrant pump from a callback — never re-fire
    }
    dispatching_ = true;

    // Snapshot the counts: callbacks may register (appends beyond the
    // snapshot wait a frame) or unregister (tombstones, kept in place).
    const std::size_t fd_count = std::min<std::size_t>(fds_.size(), 32);
    if (fd_count > 0) {
        std::array<pollfd, 32> poll_set{};
        for (std::size_t i = 0; i < fd_count; ++i) {
            poll_set[i].fd = fds_[i].alive ? fds_[i].fd : -1; // -1 = ignored by poll
            poll_set[i].events = POLLIN;
        }
        if (poll(poll_set.data(), static_cast<nfds_t>(fd_count), 0) > 0) {
            for (std::size_t i = 0; i < fd_count; ++i) {
                if (fds_[i].alive && poll_set[i].revents != 0) {
                    fds_[i].handler->onFDIsSet(fds_[i].fd);
                }
            }
        }
    }

    const double now = now_seconds();
    const std::size_t timer_count = timers_.size();
    for (std::size_t i = 0; i < timer_count; ++i) {
        TimerEntry& entry = timers_[i];
        if (!entry.alive || now < entry.due) {
            continue;
        }
        // Deadline advances before the callback so a slow handler (or
        // a second dispatch this frame) cannot double-fire.
        entry.due = now + entry.interval_s;
        entry.handler->onTimer();
    }

    dispatching_ = false;
    compact();
}

void Vst3RunLoop::compact() {
    for (FdEntry& entry : fds_) {
        if (!entry.alive) {
            entry.handler->release();
        }
    }
    std::erase_if(fds_, [](const FdEntry& entry) { return !entry.alive; });
    for (TimerEntry& entry : timers_) {
        if (!entry.alive) {
            entry.handler->release();
        }
    }
    std::erase_if(timers_, [](const TimerEntry& entry) { return !entry.alive; });
}

} // namespace nt::ext

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

#else // _WIN32

// VST3 views on Windows ride the global message loop — no host run
// loop exists (Research/07). The declaration keeps this TU non-empty
// for MSVC.
namespace nt::ext {}

#endif // !_WIN32
