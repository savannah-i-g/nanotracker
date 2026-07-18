#include "rt/rt_assert.h"

#ifndef NDEBUG

#include <cstdio>
#include <cstdlib>
#include <new>

namespace nt::rt {

namespace {

// Depth counter (not a flag) so nested RtScopes compose.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) — per-thread state
thread_local int t_rt_depth = 0;

} // namespace

bool in_rt_context() {
    return t_rt_depth > 0;
}

RtScope::RtScope() {
    ++t_rt_depth;
}

RtScope::~RtScope() {
    --t_rt_depth;
}

} // namespace nt::rt

namespace {

void abort_rt_violation(const char* what) {
    // fprintf is itself not RT-safe, but the process is aborting — the
    // diagnostic matters more than the deadline at this point.
    std::fprintf(stderr, "RT violation: %s on the real-time thread\n", what);
    std::abort();
}

} // namespace

// Global allocation hooks: route every allocation through the RT check.
// Debug builds only (see header); the checks cost one thread-local read
// per allocation. This file IS the allocator layer, so the manual
// malloc/free management checks are suppressed wholesale.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

void* operator new(std::size_t size) {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator new");
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator new[]");
    }
    void* p = std::malloc(size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete(void* p) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete");
    }
    std::free(p);
}

void operator delete[](void* p) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete[]");
    }
    std::free(p);
}

void operator delete(void* p, std::size_t size) noexcept {
    static_cast<void>(size);
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete");
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t size) noexcept {
    static_cast<void>(size);
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete[]");
    }
    std::free(p);
}

// Nothrow variants: the library uses these too (e.g. stable_sort's
// temporary buffer via get_temporary_buffer). Overriding only the
// throwing forms would pair the default nothrow new with our free()
// — an alloc/dealloc mismatch under ASan and a hole in the RT check.

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator new(nothrow)");
    }
    return std::malloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator new[](nothrow)");
    }
    return std::malloc(size);
}

void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete(nothrow)");
    }
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept {
    if (nt::rt::in_rt_context()) {
        abort_rt_violation("operator delete[](nothrow)");
    }
    std::free(p);
}

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

#endif // NDEBUG
