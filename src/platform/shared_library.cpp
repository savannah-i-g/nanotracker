#include "platform/shared_library.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nt::platform {

#ifdef _WIN32

void* library_open(const std::filesystem::path& path, std::string& error) {
    HMODULE handle = ::LoadLibraryW(path.c_str());
    if (handle == nullptr) {
        error =
            "LoadLibrary failed (code " + std::to_string(::GetLastError()) + "): " + path.string();
    }
    return handle;
}

void* library_symbol(void* handle, const char* name) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — FARPROC contract
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
}

void library_close(void* handle) {
    if (handle != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(handle));
    }
}

#else

void* library_open(const std::filesystem::path& path, std::string& error) {
    void* handle = ::dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (handle == nullptr) {
        const char* why = ::dlerror();
        error = why != nullptr ? why : ("dlopen failed: " + path.string());
    }
    return handle;
}

void* library_symbol(void* handle, const char* name) {
    return ::dlsym(handle, name);
}

void library_close(void* handle) {
    if (handle != nullptr) {
        ::dlclose(handle);
    }
}

#endif

} // namespace nt::platform
