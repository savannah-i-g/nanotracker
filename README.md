# NanoTracker

A native music tracker for Linux and Windows: pattern-based
sequencing, a patchable workspace of audio/CV/gate/MIDI cables, a
declarative plugin format (NTP), CLAP and VST3 hosting, MIDI I/O with
a phase-locked MIDI clock, module (MOD/XM/S3M/IT) import, and offline
WAV/OGG/MP3 export — built on OpenAL Soft, OpenGL, and Dear ImGui
(docking).

NanoTracker began as a web application; this repository is the native
port and the project's home going forward. The web behaviour is the
reference, not the implementation: where the web version was broken,
the native port fixes it and records the divergence in
`Docs/FIXES.md`.

**Status:** Linux is the reference platform. The Windows build is
**beta** — it is CI-built and tested but has not yet been confirmed on
real hardware by enough users. Reports welcome.

## Building

Requirements: CMake ≥ 3.24, a C++20 compiler (GCC on Linux, MSVC 2022
on Windows), Python 3 with `jinja2` (the GL loader is generated at
configure time). Most dependencies are fetched and pinned by CMake;
the full roster with versions and licences is `Docs/DEPENDENCIES.md`.

### Linux

System packages (Debian/Ubuntu names): `libopenal-dev`,
`libasound2-dev`, `libx11-dev`, `libgl1-mesa-dev`, `python3-jinja2`,
and either `libopenmpt-dev` or a vendored build via
`tools/build_libopenmpt.sh`. Optional: `libmp3lame0` enables MP3
export at run time.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/nanotracker
```

### Windows (beta)

MSVC 2022 (the VST3 SDK does not support MinGW). OpenAL Soft is built
from source and shipped beside the executable — the stock
`OpenAL32.dll` lacks the callback extension the audio device requires.
libopenmpt comes from the upstream prebuilt development package,
pinned by hash in `CMakeLists.txt`.

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or download the CI-built beta artifact from the Actions page /
releases.

## Tests

The Catch2 suite (`tests/`) covers the engine golden traces, the
graph compiler, the FTRK format round-trip, importers against binary
fixtures, plugin hosting against an in-tree CLAP fixture, MIDI (live
when a loopback device exists, WARN-skipped headless), and offline
export. Tests that need real devices skip cleanly on CI.

## Licence

GPLv3 (see `LICENSE`), with one deliberate exception: the NTP plugin
format headers under `include/ntp/` are MIT-licensed so plugin
authors can consume them without licence entanglement. Third-party
dependency licences are collected in `LICENSES/`.
