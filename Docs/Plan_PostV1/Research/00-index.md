# Research Index — Post-v1 Burst 2 (2026-07-18)

Web-research grounding for the post-v1 plan. Each topic file carries
findings, plan implications, and sources. This index separates what
*changes* the plan from what *confirms* it.

## Read order

1. `01-ci-github-actions.md` — CI toolchain, caching, headless GL
2. `02-libopenmpt-windows.md` — acquisition path on Windows
3. `03-websocket-lib.md` — Local API server library
4. `04-convolution-fft.md` — FFT library + partitioned convolution
5. `05-lufs-libebur128.md` — loudness measurement library
6. `06-packaging.md` — Linux + Windows distribution formats
7. `07-plugin-gui-hosting.md` — CLAP Win32 + VST3 IRunLoop contracts

## Would-change-the-plan items

1. **Windows toolchain is MSVC, not MinGW** — the VST3 SDK is
   effectively MSVC-only on Windows (`__uuidof`, runtime crashes under
   MinGW, Steinberg recommends VS). Stage 13 CI locks MSVC.
   → applied to `../02-release-engineering.md`
2. **libopenmpt via upstream prebuilt, not vcpkg** — the vcpkg port is
   unmaintained and called broken by the upstream maintainer; upstream
   ships VS2022 dev packages (headers + import libs, amd64/x86).
   → applied to `../02-release-engineering.md`
3. **Headless GL on CI needs Mesa on both runners** — GitHub runners
   have no GPU; Linux = xvfb + llvmpipe, Windows = mesa-dist-win
   `opengl32.dll` beside the test binary. Input-script UI tests stay
   runnable on CI. → applied to `../02-release-engineering.md`
4. **Local API library: IXWebSocket (BSD-3-Clause)** — client+server,
   minimal deps, TLS optional (unused; localhost), MSVC-tested CMake
   build. Maintenance is slowing (maintainer note, 2026-06) — pin a
   release; acceptable risk for a localhost control surface.
   → applied to `../07-local-api.md`
5. **Linux packaging: AppImage primary, Flatpak parked** — Flatpak's
   sandbox fights a plugin host (system CLAP/VST3 dirs, arbitrary
   user dlopen); AppImage keeps host-filesystem semantics. Flatpak
   recorded as a community follow-up, not a Stage 21 deliverable.
   → applied to `../10-native-stage-abi.md`

## Confirmations (plan stands)

- **pffft for FFT** (BSD-3-like, SIMD, C++ wrapper) — Stage 20's
  uniform-partitioned-first design is the literature default
  (Torger/Farina 2001); FFTConvolver (MIT) is a proven reference
  implementation of exactly that scheme. Named in
  `../09-plugin-platform.md`.
- **libebur128 for LUFS** (MIT, conformance-passing, true peak + LRA
  included) — use it, do not self-implement BS.1770. Named in
  `../04-export-suite.md`.
- **OpenAL Soft must be built/shipped ourselves on Windows** — no
  first-party prebuilts; vcpkg port exists (1.25.1) but has a recent
  MSVC-2026 build-failure report; FetchContent is the Windows
  acquisition route (Linux keeps the system package).
- **Windows beta artifact = portable zip** — winget accepts zip
  manifests since 1.4, so the beta zip can graduate to a winget
  manifest at GA without changing the artifact shape.
- **VST3 IRunLoop event handlers must run on the UI thread** (JUCE
  plugins assume it) — our CLAP pump already dispatches from the
  frame loop, so the Stage 20 IRunLoop rides the same discipline.
- **CLAP Win32 embedding is the simple case** — `SetParent` +
  `CLAP_WINDOW_API_WIN32`; the Stage 13 editor-window abstraction
  needs nothing exotic on the Win32 side.

## Applied log

- [x] `../02-release-engineering.md` — MSVC lock, libopenmpt upstream
      prebuilt, Mesa headless GL, ccache/sccache + FetchContent cache
- [x] `../04-export-suite.md` — libebur128 named (MIT)
- [x] `../07-local-api.md` — IXWebSocket named (BSD-3, pinned release)
- [x] `../09-plugin-platform.md` — pffft named; IRunLoop UI-thread note
- [x] `../10-native-stage-abi.md` — AppImage primary / Flatpak parked;
      Windows zip → winget at GA
