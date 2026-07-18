// ext/vst3_run_loop — the host-side Steinberg::Linux::IRunLoop. On
// Linux VST3 defines no global event loop, so the host must hand
// plugins one; plugins reach it through BOTH documented routes (each
// must work — Research/07): queryInterface on the IPlugFrame passed to
// IPlugView::setFrame (ext/vst3_editor_window), and queryInterface on
// the host context set with IPluginFactory3::setHostContext
// (ext/vst3_host) — the latter is how a plugin gets a run loop before
// it has any editor.
//
// Threading contract (the Research/07 load-bearing finding): every
// callback fires on the UI thread, from dispatch() in the frame loop —
// JUCE-built plugins assume it and a worker poll thread deadlocks
// them. Registration is UI-thread too (setFrame/attached and factory
// calls all happen there), so no locking exists on purpose.
//
// Windows needs none of this (views ride the global message loop);
// the whole file compiles away there, like the CLAP posix-fd pump.
#pragma once
#ifndef _WIN32

#include <pluginterfaces/gui/iplugview.h>
#include <vector>

namespace nt::ext {

// Process-wide singleton: handlers are keyed by plugin-owned handler
// objects, not by window, because the factory-context route registers
// without a window. dispatch() runs once per UI frame from the
// session's editor sweep whether or not any editor is open.
class Vst3RunLoop final : public Steinberg::Linux::IRunLoop {
public:
    static Vst3RunLoop& instance();

    // ── IRunLoop (UI thread) ─────────────────────────────────────────
    Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* handler,
                                                       Steinberg::Linux::FileDescriptor fd) final;
    Steinberg::tresult PLUGIN_API
    unregisterEventHandler(Steinberg::Linux::IEventHandler* handler) final;
    Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* handler,
                                                Steinberg::Linux::TimerInterval milliseconds) final;
    Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* handler) final;

    // ── FUnknown ─────────────────────────────────────────────────────
    // Static-lifetime host object: refcounts are a formality (the COM
    // seam), never a deleter.
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID interface_id,
                                                 void** obj) final;

    Steinberg::uint32 PLUGIN_API addRef() final { return 1; }

    Steinberg::uint32 PLUGIN_API release() final { return 1; }

    // Once per UI frame: polls registered fds (zero timeout) and fires
    // due timers. Guards against the two known host-bug classes
    // (Research/07): handlers unregistered mid-dispatch are only
    // marked dead and compacted after the pass, and a re-entrant
    // dispatch (a callback pumping the loop again) returns
    // immediately instead of re-firing timers.
    void dispatch();

    [[nodiscard]] std::size_t handler_count() const { return fds_.size() + timers_.size(); }

private:
    Vst3RunLoop() = default;

    struct FdEntry {
        Steinberg::Linux::IEventHandler* handler = nullptr; // ref held while alive
        int fd = -1;
        bool alive = true;
    };

    struct TimerEntry {
        Steinberg::Linux::ITimerHandler* handler = nullptr; // ref held while alive
        double interval_s = 0.0;
        double due = 0.0; // steady-clock seconds
        bool alive = true;
    };

    void compact();

    std::vector<FdEntry> fds_;
    std::vector<TimerEntry> timers_;
    bool dispatching_ = false;
};

} // namespace nt::ext

#endif // !_WIN32
