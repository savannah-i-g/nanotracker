# Dependencies

Every third-party dependency with pinned version and license. Updated
the moment a dependency is added or bumped. Planned set (versions pinned
at Stage 0):

| Dependency | Role | License | Pinned version |
| --- | --- | --- | --- |
| GLFW | window/context/input | zlib | 3.4 (FetchContent tag) |
| glad | GL loader | MIT | v2.0.8 (FetchContent tag; generates gl:core=3.3) |
| Dear ImGui (docking) | UI | MIT | v1.92.8-docking (FetchContent tag) |
| OpenAL Soft | audio device | LGPL (dyn) | Linux: system package 1.23.1 (find_package). Windows: FetchContent tag 1.25.2, built and shipped beside the exe (stock OpenAL32.dll lacks the callback extension). Verified at runtime either way |
| libopenmpt | module playback | BSD-3 | Linux: 0.8.3 vendored static (tools/build_libopenmpt.sh; third_party/, excluded from backups) or system pkg-config (CI). Windows: 0.8.3 upstream VS2022 dev package, URL_HASH-pinned in CMakeLists.txt |
| RtMidi | MIDI I/O | MIT-like | 6.0.0 (FetchContent tag; ALSA backend, libasound2-dev installed 2026-07-18) |
| IXWebSocket | Local API WebSocket server + test loopback client | BSD-3 | v12.0.1 (FetchContent tag; USE_TLS/USE_ZLIB forced off — localhost + bearer token, no new transitive deps) |
| dr_libs (dr_wav + dr_mp3) | sample decode | PD/MIT | commit 6d78776c2c05 (FetchContent) |
| stb (stb_vorbis + stb_image) | ogg decode, plugin UI images | PD/MIT | commit f0569113 (FetchContent; decoders.cpp + plugins/image_decode.cpp TUs, warnings suppressed) |
| libogg + libvorbis | ogg encode | BSD | v1.3.5 / v1.3.7 (FetchContent tags) |
| LAME | mp3 encode | LGPL (dyn) | system libmp3lame.so.0, dlopen'd at run time (no build dependency; export reports cleanly when absent) |
| libsamplerate | resampling | BSD-2 | 0.2.2 (FetchContent tag; BUILD_TESTING forced off) |
| miniz | plugin archives | MIT | 3.0.2 (FetchContent tag) |
| libebur128 | LUFS / true-peak measurement (export normalise) | MIT | v1.2.6 (FetchContent tag; single ebur128.c compiled as a static target, bundled sys/queue.h shim on all platforms) |
| nlohmann-json | manifests/settings | MIT | v3.12.0 (FetchContent tag) |
| Catch2 | tests | BSL-1.0 | v3.7.1 (FetchContent tag) |
| CLAP headers | plugin hosting | MIT | 1.2.2 (FetchContent tag, interface lib) |
| VST3 SDK | plugin hosting | MIT (relicensed Nov 2025) | v3.7.9_build_61 (FetchContent; hosting sources compiled directly, SDK build system bypassed) |
| Kode Mono font | UI font | SIL OFL 1.1 | static TTFs (Regular/Medium/SemiBold/Bold) in assets/fonts, OFL bundled |
