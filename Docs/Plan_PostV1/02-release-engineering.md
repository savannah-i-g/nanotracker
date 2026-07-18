# 02 — Release Engineering (Stage 13)

The opening move: the repo goes public with CI on both platforms, and
the Windows port lands as an explicitly-labeled beta. CI *is* the
Windows toolchain (locked decision 1) — every later stage keeps both
platforms green instead of accumulating a second port at the end.

## Repo + publish scrub

- `git init` in `Output/` — tar backups continue as belt-and-braces;
  the stage-close ritual becomes verify → ledgers → git tag + tar.
- `.gitignore`: `build*/`, `third_party/`, editor droppings.
- `README.md`: what it is, build steps (Linux + Windows), dependency
  notes (OpenAL Soft requirement, libopenmpt acquisition, optional
  libmp3lame), licence pointers.
- `LICENSES/`: add the ~14 missing third-party texts (the roster is
  `Docs/DEPENDENCIES.md`; only Kode Mono OFL and NTP-MIT exist).
- Public repository (GPLv3 from day one, per v1 locked decision).

## CI

GitHub Actions, two jobs minimum:
- **Linux** (ubuntu-latest): configure, build `nanotracker` +
  `nt_tests` + `ntp-convert`, run ctest. Device-dependent tests
  already WARN-skip headless — CI-safe by construction.
- **Windows** (windows-latest): **MSVC** — locked, not a decision
  point anymore: the VST3 SDK is effectively MSVC-only on Windows
  (`__uuidof`, MinGW runtime crashes; Research/07, applied
  2026-07-18). OpenAL Soft via FetchContent, same as Linux (no
  first-party prebuilts; vcpkg adds a moving part — Research/01).
  libopenmpt per Research/02 (upstream prebuilt). Build + ctest,
  upload the beta artifact.
- **Caching + headless GL** (Research/01, applied 2026-07-18):
  ccache (Linux) / sccache (Windows) via `hendrikmuhs/ccache-action`;
  `actions/cache` over `_deps` sources (2 GiB repo budget — cache
  sources + compiler cache, never build trees). Runners have no GPU:
  input-script UI tests run under xvfb + llvmpipe on Linux and with
  mesa-dist-win's software `opengl32.dll` beside the binary on
  Windows.

## The Windows seam (complete audited surface)

Four source files + the build system. No other file in `src/`
touches a platform API outside the existing `platform/` layer.

- `src/platform/paths.cpp` — `/proc/self/exe` → `GetModuleFileNameW`;
  XDG/HOME → `%APPDATA%`. This file is the intended `#ifdef` seam.
- `src/ext/editor_window.{h,cpp}` — introduce a thin platform
  abstraction over "top-level OS window a plugin embeds into":
  X11 impl (current code) + Win32 impl (`CreateWindowEx`,
  `CLAP_WINDOW_API_WIN32`, `PeekMessage` pump, `WM_CLOSE`).
  Stage 20's VST3 IPlugView editors inherit this seam — design it
  for two clients, build it for one.
- `src/ext/clap_host.cpp` — dlopen/dlsym/dlclose behind a tiny
  shared-library wrapper (→ `LoadLibraryW`/`GetProcAddress`);
  posix-fd-support compiled out on Windows (timer-support carries
  plugin GUIs there); search paths gain `%COMMONPROGRAMFILES%\CLAP`
  and `%LOCALAPPDATA%\Programs\Common\CLAP`, `CLAP_PATH` splits on
  `;`.
- `src/io/export_render.cpp` — the lame loader uses the same
  shared-library wrapper (`libmp3lame.dll` / `lame_enc.dll`);
  MP3 export stays cleanly optional.
- `CMakeLists.txt` + `tests/CMakeLists.txt` — gate
  `find_package(X11)` + `X11::X11` on UNIX; swap the VST3 hosting
  TUs (`module_linux.cpp`/`threadchecker_linux.cpp` →
  `module_win32.cpp`/`threadchecker_win32.cpp`) on WIN32; gate the
  GCC-only warning flags; OpenAL Soft shipped/vendored on Windows
  (callback extension is a hard requirement); libopenmpt from the
  pinned upstream VS2022 development package — headers + import lib
  + DLL; the vcpkg port is ruled out as unmaintained/broken
  (Research/02, applied 2026-07-18). glad needs python3+jinja2 on
  the runner.

## Fold-ins

- S3M/IT importer binary fixtures + oracle tests (copy the MOD/XM
  pattern in `tests/mod_import_test.cpp` / `xm_import_test.cpp`) —
  importers are endianness/path bug farms; this coverage belongs on
  the Windows runner from the first green build.
- Fix the three stale promise comments (`01-context.md` §A) — a
  comment promising work is a documentation bug.

## Deliverable + non-goals

Deliverable: public repo, green CI on both platforms, a downloadable
Windows artifact labeled **BETA** (locked decision 4: it stays beta
until community testers confirm it; Wine boots locally count as a
smoke proxy, recorded as proxy, never as validation).

Non-goal (recorded): the OpenAL buffer-queue fallback stays unbuilt.
Shipping OpenAL Soft dissolves its rationale; revisit only if beta
reports callback problems.
