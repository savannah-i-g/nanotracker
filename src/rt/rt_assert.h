// rt/rt_assert — real-time-safety guard rails.
// The audio thread must never allocate, lock, or block. Debug builds
// enforce the allocation rule mechanically: while an RtScope is alive
// on a thread, any operator new/delete on that thread aborts with a
// diagnostic. Release builds compile all of this away.
#pragma once

namespace nt::rt {

#ifndef NDEBUG

// True while the current thread is inside an RtScope.
bool in_rt_context();

// Marks the enclosing scope as real-time: allocations on this thread
// abort until destruction. Nesting is allowed.
class RtScope {
public:
    RtScope();
    ~RtScope();

    RtScope(const RtScope&) = delete;
    RtScope& operator=(const RtScope&) = delete;
    RtScope(RtScope&&) = delete;
    RtScope& operator=(RtScope&&) = delete;
};

#else

inline bool in_rt_context() {
    return false;
}

class RtScope {
public:
    RtScope() = default;
};

#endif

} // namespace nt::rt
