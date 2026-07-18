#include "platform/paths.h"

#include <cstdlib>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace nt::platform {

namespace fs = std::filesystem;

namespace {

fs::path home_dir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home != nullptr ? fs::path(home) : fs::current_path();
}

// Directory containing the running executable; empty if unresolvable.
fs::path executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return fs::path(buffer.substr(0, length)).parent_path();
#else
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : exe.parent_path();
#endif
}

} // namespace

fs::path config_dir() {
    fs::path base;
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        base = fs::path(appdata);
    } else {
        base = home_dir();
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        base = fs::path(xdg);
    } else {
        base = home_dir() / ".config";
    }
#endif
    fs::path dir = base / "nanotracker";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path find_asset(const fs::path& relative) {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("NANOTRACKER_ASSETS"); env != nullptr && *env != '\0') {
        roots.emplace_back(env);
    }
    if (const fs::path exe_dir = executable_dir(); !exe_dir.empty()) {
        roots.push_back(exe_dir / "assets");
        roots.push_back(exe_dir.parent_path() / "assets");
    }
    roots.push_back(fs::current_path() / "assets");

    for (const fs::path& root : roots) {
        fs::path candidate = root / relative;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return candidate;
        }
    }
    throw std::runtime_error("asset not found: " + relative.string());
}

} // namespace nt::platform
