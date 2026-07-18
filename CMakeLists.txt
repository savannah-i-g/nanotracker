# NanoTracker — native tracker application.
# Single top-level target plus tests. Third-party dependencies are
# fetched with pinned tags (see Docs/DEPENDENCIES.md); libopenmpt is the
# one exception (pkg-config, cmake/Findlibopenmpt.cmake).
cmake_minimum_required(VERSION 3.24)

project(NanoTracker
    VERSION 0.1.0
    DESCRIPTION "NanoTracker — native music tracker"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(NT_BUILD_TESTS "Build the test suite" ON)
option(NT_SANITIZE "Build with ASan/UBSan" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Third-party (pinned) ─────────────────────────────────────────────
# CMake ≥ 4 removed compatibility with sub-3.5 minimums; miniz 3.0.2
# (the last release) declares one. This floors dependency policies
# without touching our own requirement above.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(glfw)

find_package(OpenGL REQUIRED)
if(UNIX)
    # Plugin editor host windows only (ext/editor_host_surface_x11);
    # app windowing is GLFW's business.
    find_package(X11 REQUIRED)
endif()

# GL loader — generated at configure time from the pinned glad2 tag
# (requires python3 + jinja2; both checked by glad's CMake).
FetchContent_Declare(glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG v2.0.8
    GIT_SHALLOW ON
    SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(glad)
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

# Dear ImGui, docking branch release tag. Compiled as a static library
# together with the GLFW + OpenGL3 backends.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8-docking
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# Consumers of glfw3.h must never receive a GL header from it — glad
# owns GL declarations, making include order irrelevant. PUBLIC here
# propagates to every target linking imgui; GLFW's own build must NOT
# see this macro (its internal.h rejects header-option macros).
target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(nlohmann_json)

# OpenAL: system OpenAL Soft on Linux; built from source and shipped
# on Windows (the stock OpenAL32.dll lacks AL_SOFT_callback_buffer,
# which the device hard-requires). LGPL satisfied by dynamic linking
# on both.
if(WIN32)
    set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openal_soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG 1.25.2
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(openal_soft)
else()
    find_package(OpenAL REQUIRED)
endif()

# Module playback. Linux: vendored static build first, system
# pkg-config second (cmake/Findlibopenmpt.cmake; build via
# tools/build_libopenmpt.sh — bash-only). Windows: the upstream
# prebuilt VS2022 development package, pinned by hash; the vcpkg port
# is unmaintained (Docs/Plan_PostV1/Research/02).
if(WIN32)
    FetchContent_Declare(libopenmpt_win
        URL "https://lib.openmpt.org/files/libopenmpt/dev/libopenmpt-0.8.3+release.dev.windows.vs2022.zip"
        URL_HASH SHA256=04323d18aa58bb91cc1feb1cc71a44aa5c1bea068ab1536a125d81c6a733acc1)
    FetchContent_MakeAvailable(libopenmpt_win)
    add_library(libopenmpt_prebuilt SHARED IMPORTED GLOBAL)
    set_target_properties(libopenmpt_prebuilt PROPERTIES
        IMPORTED_LOCATION ${libopenmpt_win_SOURCE_DIR}/bin/amd64/libopenmpt.dll
        IMPORTED_IMPLIB ${libopenmpt_win_SOURCE_DIR}/lib/amd64/libopenmpt.lib
        INTERFACE_INCLUDE_DIRECTORIES ${libopenmpt_win_SOURCE_DIR}/inc)
    add_library(libopenmpt::libopenmpt ALIAS libopenmpt_prebuilt)
    # The DLL set the executable needs beside it (libopenmpt + its
    # bundled codec DLLs).
    set(NT_LIBOPENMPT_RUNTIME
        ${libopenmpt_win_SOURCE_DIR}/bin/amd64/libopenmpt.dll
        ${libopenmpt_win_SOURCE_DIR}/bin/amd64/openmpt-mpg123.dll
        ${libopenmpt_win_SOURCE_DIR}/bin/amd64/openmpt-ogg.dll
        ${libopenmpt_win_SOURCE_DIR}/bin/amd64/openmpt-vorbis.dll
        ${libopenmpt_win_SOURCE_DIR}/bin/amd64/openmpt-zlib.dll)
else()
    find_package(libopenmpt REQUIRED)
endif()

# dr_libs is header-only with no build system: populate + interface lib.
FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG 6d78776c2c05)
FetchContent_MakeAvailable(dr_libs)
add_library(dr_libs INTERFACE)
target_include_directories(dr_libs SYSTEM INTERFACE ${dr_libs_SOURCE_DIR})

# stb (stb_vorbis, stb_image) — header-only, pinned commit, no build system.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG f0569113c93ad095470c54bf34a17b36646bbbb5)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# miniz — NTP plugin archives (.ntins/.ntsfx are ZIP files). MIT.
set(MINIZ_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(miniz)

# libogg + libvorbis — OGG export encoding. BSD.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(ogg)
set(OGG_INCLUDE_DIR ${ogg_SOURCE_DIR}/include CACHE PATH "" FORCE)
set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
FetchContent_Declare(vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(vorbis)

# RtMidi — MIDI I/O (ALSA on Linux). MIT-like licence.
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
# GLFW already owns the "uninstall" custom target.
set(RTMIDI_TARGETNAME_UNINSTALL rtmidi_uninstall CACHE STRING "" FORCE)
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG 6.0.0
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(rtmidi)

# CLAP — external plugin hosting ABI (header-only C). MIT.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.2
    GIT_SHALLOW ON)
FetchContent_Populate(clap)
add_library(clap INTERFACE)
target_include_directories(clap SYSTEM INTERFACE ${clap_SOURCE_DIR}/include)

# VST3 SDK — hosting only (MIT since Nov 2025). The SDK's own build
# system is bypassed: the hosting-support sources are compiled directly
# into a small static library (the approach open-source hosts take).
FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.7.9_build_61
    GIT_SHALLOW ON
    GIT_SUBMODULES "base;pluginterfaces;public.sdk")
FetchContent_Populate(vst3sdk)
if(WIN32)
    set(NT_VST3_PLATFORM_SOURCES
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/threadchecker_win32.cpp)
else()
    set(NT_VST3_PLATFORM_SOURCES
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/threadchecker_linux.cpp)
endif()
add_library(vst3_hosting STATIC
    ${NT_VST3_PLATFORM_SOURCES}
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/processdata.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/utility/stringconvert.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/memorystream.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/fobject.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/baseiids.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/fstring.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/fbuffer.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/updatehandler.cpp
    ${vst3sdk_SOURCE_DIR}/base/source/fdebug.cpp
    ${vst3sdk_SOURCE_DIR}/base/thread/source/flock.cpp
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstinitiids.cpp
    ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/coreiids.cpp
    ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/funknown.cpp
    ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/ustring.cpp
    ${vst3sdk_SOURCE_DIR}/pluginterfaces/base/conststringtable.cpp)
target_include_directories(vst3_hosting SYSTEM PUBLIC ${vst3sdk_SOURCE_DIR})
target_compile_options(vst3_hosting PRIVATE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
target_compile_definitions(vst3_hosting PUBLIC RELEASE=1)
target_link_libraries(vst3_hosting PUBLIC ${CMAKE_DL_LIBS})

set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
# Keep third-party test suites out of our ctest registry.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG 0.2.2
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(libsamplerate)

# ── Application ──────────────────────────────────────────────────────
# Plugin editor host windows: one surface implementation per platform
# (ext/editor_host_surface.h).
if(WIN32)
    set(NT_EDITOR_SURFACE_SOURCE src/ext/editor_host_surface_win32.cpp)
else()
    set(NT_EDITOR_SURFACE_SOURCE src/ext/editor_host_surface_x11.cpp)
endif()

add_executable(nanotracker
    ${NT_EDITOR_SURFACE_SOURCE}
    src/app/main.cpp
    src/app/input_script.cpp
    src/app/pattern_ops.cpp
    src/app/project_session.cpp
    src/app/autosave.cpp
    src/app/undo.cpp
    src/ui/cable_overlay.cpp
    src/ui/fx_mixer_view.cpp
    src/ui/instrument_table_view.cpp
    src/ui/pattern_view.cpp
    src/ui/sample_view.cpp
    src/ui/ntp_ui.cpp
    src/ui/piano_roll_view.cpp
    src/ui/midi_view.cpp
    src/ui/workspace_view.cpp
    src/audio/audio_device_openal.cpp
    src/audio/audio_engine.cpp
    src/audio/decoders.cpp
    src/audio/fx_chain.cpp
    src/audio/graph_runner.cpp
    src/audio/sample_buffer.cpp
    src/engine/sequence_ops.cpp
    src/engine/tracker_engine.cpp
    src/ext/clap_host.cpp
    src/ext/editor_window.cpp
    src/ext/vst3_host.cpp
    src/graph/graph_compile.cpp
    src/graph/graph_model.cpp
    src/graph/graph_wpbr.cpp
    src/plugins/image_decode.cpp
    src/plugins/ntp_graph.cpp
    src/plugins/ntp_loader.cpp
    src/plugins/plugin_registry.cpp
    src/plugins/ntp_voices.cpp
    src/plugins/preset_bank.cpp
    src/io/ftrk_reader.cpp
    src/io/ftrk_writer.cpp
    src/io/export_render.cpp
    src/io/import/import_common.cpp
    src/io/import/it_importer.cpp
    src/io/import/mod_importer.cpp
    src/io/import/s3m_importer.cpp
    src/io/import/xm_importer.cpp
    src/io/settings.cpp
    src/midi/midi_io.cpp
    src/midi/midi_learn.cpp
    src/midi/midi_out_thread.cpp
    src/modplay/module_player.cpp
    src/ui/module_view.cpp
    src/platform/app_window.cpp
    src/platform/imgui_context.cpp
    src/platform/paths.cpp
    src/platform/shared_library.cpp
    src/rt/rt_assert.cpp
    src/ui/crt_pass.cpp
    src/ui/theme.cpp)

target_include_directories(nanotracker PRIVATE src include)
target_link_libraries(nanotracker PRIVATE
    glfw OpenGL::GL imgui glad_gl_core_33 nlohmann_json::nlohmann_json
    OpenAL::OpenAL dr_libs stb samplerate libopenmpt::libopenmpt miniz
    clap vst3_hosting rtmidi vorbisenc vorbis ogg ${CMAKE_DL_LIBS}
    $<$<BOOL:${UNIX}>:X11::X11>)

# Third-party single-file codec implementations: warnings are theirs.
set(NT_SILENCE_WARNINGS $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_source_files_properties(src/audio/decoders.cpp
    PROPERTIES COMPILE_OPTIONS "${NT_SILENCE_WARNINGS}")
set_source_files_properties(src/plugins/image_decode.cpp
    PROPERTIES COMPILE_OPTIONS "${NT_SILENCE_WARNINGS}")

if(MSVC)
    target_compile_options(nanotracker PRIVATE /W4 /utf-8 /bigobj)
else()
    target_compile_options(nanotracker PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# Windows runtime set: the shipped/tested binary needs OpenAL Soft and
# the libopenmpt DLLs beside it.
if(WIN32)
    add_custom_command(TARGET nanotracker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL> ${NT_LIBOPENMPT_RUNTIME}
            $<TARGET_FILE_DIR:nanotracker>)
endif()

if(NT_SANITIZE)
    target_compile_options(nanotracker PRIVATE -fsanitize=address,undefined)
    target_link_options(nanotracker PRIVATE -fsanitize=address,undefined)
endif()

# ── Tools ────────────────────────────────────────────────────────────
# Web plugin manifest → NTP v1 converter (JSON in, JSON out).
add_executable(ntp-convert tools/ntp-convert/main.cpp)
target_link_libraries(ntp-convert PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(ntp-convert PRIVATE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall;-Wextra>)

# ── Tests ────────────────────────────────────────────────────────────
if(NT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
