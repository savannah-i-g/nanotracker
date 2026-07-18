// Application identity constants. The single source of truth for the
// version string; CMake's project() version is kept in sync manually
// and checked by the smoke test.
#pragma once

namespace nt {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr const char* kVersionString = "1.0.0";
inline constexpr const char* kAppName = "nanoTracker";

} // namespace nt
