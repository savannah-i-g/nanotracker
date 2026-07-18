// platform/shared_library — runtime library loading behind one seam
// (dlopen on POSIX, LoadLibrary on Windows). Every runtime-loaded
// binary goes through here: CLAP plugin files, the optional lame
// encoder. Handles are opaque; callers cast symbol() results per the
// dlsym/GetProcAddress contract.
#pragma once

#include <filesystem>
#include <string>

namespace nt::platform {

// Loads a shared library with local symbol visibility and immediate
// binding. Returns nullptr with `error` set on failure.
void* library_open(const std::filesystem::path& path, std::string& error);

// Resolves an exported symbol; nullptr when absent.
void* library_symbol(void* handle, const char* name);

// Safe on nullptr.
void library_close(void* handle);

} // namespace nt::platform
