# 01 — CI on GitHub Actions for a C++ Audio App

Grounds Stage 13 (`../02-release-engineering.md`).

## Headline facts

| Question | Answer |
| --- | --- |
| Windows toolchain | MSVC (VS2022 on `windows-latest`) — MinGW ruled out by the VST3 SDK (see `07-plugin-gui-hosting.md`) |
| Compiler cache | ccache (Linux) / sccache (Windows — stable Windows support) via `hendrikmuhs/ccache-action` |
| Repo cache budget | 2 GiB per repository (GitHub Actions cache) |
| FetchContent caching | cache the build tree's `_deps` source dirs with `actions/cache`; CPM.cmake's `CPM_SOURCE_CACHE` is the same idea if we ever want it — plain FetchContent + cache action suffices |
| GPU on runners | none, either OS — software GL required for UI tests |
| OpenAL Soft on runners | Linux: distro package (as locally). Windows: build from source via FetchContent — no first-party prebuilts; vcpkg port (1.25.1) has a 2026 MSVC build-failure report — avoid the extra moving part |

## Headless GL

GitHub runners have no GPU. The input-script harness (our only UI
automation) needs a real GL context:

- **Linux**: `xvfb-run` + Mesa llvmpipe (distro Mesa already ships
  it). This is the standard pattern for GLFW/OpenGL tests on Actions.
- **Windows**: drop Mesa3D's software `opengl32.dll` next to the test
  binary — `pal1000/mesa-dist-win` publishes prebuilt releases used
  exactly this way in CI. llvmpipe translates shaders through LLVM to
  x86-64; ImGui-scale rendering is well within its envelope.

Device-dependent audio/MIDI tests already WARN-skip when no device
exists, so the suites are CI-safe by construction.

## Caching strategy (Stage 13 concrete)

- `hendrikmuhs/ccache-action` per job (ccache on Linux, sccache on
  Windows), keyed by compiler + OS.
- `actions/cache` over `build/_deps/*-src` keyed by the dependency
  list hash, so OpenAL Soft / GLFW / libopenmpt sources are not
  re-fetched per run.
- Stay under the 2 GiB repo budget: cache sources + compiler cache,
  never build trees.

## Plan implications

- Stage 13 CI matrix: `ubuntu-latest` (GCC) + `windows-latest`
  (MSVC), both running the full 37-test suite with Mesa-backed GL.
- MSVC will surface warnings GCC never saw — treat the first Windows
  build as a porting task (the plan already budgets this), and gate
  GCC-only flags in CMake.
- The Windows job produces the BETA zip artifact (see
  `06-packaging.md`).

## Sources

- https://github.com/hendrikmuhs/ccache-action
- https://cristianadam.eu/20200113/speeding-up-c-plus-plus-github-actions-using-ccache/
- https://raymii.org/s/articles/Github_Actions_cpp_boost_cmake_speedup.html
- https://github.com/cpm-cmake/CPM.cmake
- https://amiralizadeh9480.medium.com/how-to-run-opengl-based-tests-on-github-actions-60f270b1ea2c
- https://github.com/pal1000/mesa-dist-win
- https://github.com/jakoch/rasterizers
- https://discourse.glfw.org/t/running-tests-in-github-actions/2143
- https://github.com/kcat/openal-soft
- https://github.com/kcat/openal-soft/issues/1060
- https://github.com/microsoft/vcpkg/issues/50596
- https://vcpkg.io/en/package/openal-soft.html
