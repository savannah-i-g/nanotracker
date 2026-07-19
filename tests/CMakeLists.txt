# Test suite. Mirrors src/ structure as modules land; golden-vector
# fixtures live under tests/golden/.
FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(catch2)

add_executable(nt_tests
    smoke.cpp
    rt_primitives.cpp
    engine_golden.cpp
    ftrk_reader_test.cpp
    mod_import_test.cpp
    xm_import_test.cpp
    s3m_import_test.cpp
    it_import_test.cpp
    session_test.cpp
    sample_ops_test.cpp
    sample_reclaim_test.cpp
    pattern_ops_test.cpp
    sequence_ops_test.cpp
    undo_test.cpp
    convolution_test.cpp
    fx_chain_test.cpp
    graph_test.cpp
    ntp_test.cpp
    onset_detect_test.cpp
    ntp_stage_test.cpp
    instance_state_test.cpp
    povr_session_test.cpp
    clap_host_test.cpp
    bridge_test.cpp
    vst3_run_loop_test.cpp
    midi_test.cpp
    midi_record_test.cpp
    ftrk_writer_test.cpp
    export_test.cpp
    export_suite_test.cpp
    export_presets_test.cpp
    local_api_test.cpp
    settings_test.cpp
    ${CMAKE_SOURCE_DIR}/src/api/local_api.cpp
    ${CMAKE_SOURCE_DIR}/src/app/midi_record.cpp
    ${CMAKE_SOURCE_DIR}/src/app/pattern_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/app/project_session.cpp
    ${CMAKE_SOURCE_DIR}/src/app/undo.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/audio_device_openal.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/audio_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/convolution_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/decoders.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/dsp_fft.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/onset_detect.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/effect_midi.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/fx_chain.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/graph_runner.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/sample_buffer.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/sample_reclaim.cpp
    ${CMAKE_SOURCE_DIR}/src/graph/graph_compile.cpp
    ${CMAKE_SOURCE_DIR}/src/graph/graph_model.cpp
    ${CMAKE_SOURCE_DIR}/src/graph/graph_wpbr.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/image_decode.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/ntp_graph.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/ntp_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/ntp_stage_host.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/plugin_registry.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/ntp_voices.cpp
    ${CMAKE_SOURCE_DIR}/src/plugins/preset_bank.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/paths.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/shared_library.cpp
    ${CMAKE_SOURCE_DIR}/${NT_EDITOR_SURFACE_SOURCE}
    ${CMAKE_SOURCE_DIR}/src/midi/midi_io.cpp
    ${CMAKE_SOURCE_DIR}/src/midi/midi_out_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/modplay/module_player.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/sequence_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/tracker_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/bridge/bridge_shm.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/bridge/bridged_plugin.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/clap_host.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/editor_window.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/vst3_editor_window.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/vst3_host.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/vst3_run_loop.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ftrk_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ftrk_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/sha256.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_post.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_presets.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_render.cpp
    ${CMAKE_SOURCE_DIR}/src/io/settings.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/import_common.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/it_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/mod_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/s3m_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/xm_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/rt/rt_assert.cpp)

# imgui headers only (no link): settings_test includes the PROJECTS /
# LIBRARY view headers for their inline list/classify helpers, and
# ui/theme.h pulls in <imgui.h>. The helpers reference no ImGui symbols,
# so the test binary still links without the UI/GL stack.
target_include_directories(nt_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include ${imgui_SOURCE_DIR})
target_link_libraries(nt_tests PRIVATE Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json OpenAL::OpenAL dr_libs stb samplerate
    libopenmpt::libopenmpt miniz clap vst3_hosting rtmidi ixwebsocket
    vorbisenc vorbis ogg ebur128 pffft
    ${CMAKE_DL_LIBS} $<$<BOOL:${UNIX}>:X11::X11>
    # bridge host-side transport: shm_open (librt) + futex/threads.
    $<$<AND:$<BOOL:${UNIX}>,$<NOT:$<BOOL:${APPLE}>>>:rt>
    $<$<BOOL:${UNIX}>:pthread>)

# bridge_test spawns the real child; it needs the built binary's path.
add_dependencies(nt_tests nanotracker-bridge-host)
target_compile_definitions(nt_tests PRIVATE
    NT_BRIDGE_HOST="$<TARGET_FILE:nanotracker-bridge-host>")

# Real .clap binary for host verification (loaded at runtime by
# clap_host_test).
add_library(nt_test_clap MODULE fixtures/test_clap_plugin.c)
target_link_libraries(nt_test_clap PRIVATE clap $<$<BOOL:${UNIX}>:m>)
set_target_properties(nt_test_clap PROPERTIES
    PREFIX "" SUFFIX ".clap" C_VISIBILITY_PRESET hidden)
target_compile_options(nt_test_clap PRIVATE
    $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
add_dependencies(nt_tests nt_test_clap)
target_compile_definitions(nt_tests PRIVATE
    NT_TEST_CLAP="$<TARGET_FILE:nt_test_clap>")

# Real native-stage binaries for the C-ABI host verification (plain
# C99 against include/ntp/ntp_stage_abi.h — both fixtures must build
# warning-free on GCC/Clang/MSVC; ntp_stage_test embeds them into
# in-memory archives under binaries/<platform-tag>/).
add_library(nt_test_stage_gain MODULE fixtures/test_stage_gain.c)
add_library(nt_test_stage_wrong_version MODULE fixtures/test_stage_wrong_version.c)
foreach(stage_fixture nt_test_stage_gain nt_test_stage_wrong_version)
    target_include_directories(${stage_fixture} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${stage_fixture} PROPERTIES
        PREFIX "" C_VISIBILITY_PRESET hidden)
endforeach()
add_dependencies(nt_tests nt_test_stage_gain nt_test_stage_wrong_version)
target_compile_definitions(nt_tests PRIVATE
    NT_TEST_STAGE_GAIN="$<TARGET_FILE:nt_test_stage_gain>"
    NT_TEST_STAGE_WRONG_VERSION="$<TARGET_FILE:nt_test_stage_wrong_version>")
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/audio/decoders.cpp
    PROPERTIES COMPILE_OPTIONS "${NT_SILENCE_WARNINGS}")
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/plugins/image_decode.cpp
    PROPERTIES COMPILE_OPTIONS "${NT_SILENCE_WARNINGS}")
if(MSVC)
    target_compile_options(nt_tests PRIVATE /utf-8 /bigobj)
endif()

# The test binary loads the same runtime DLL set as the app.
if(WIN32)
    add_custom_command(TARGET nt_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL> ${NT_LIBOPENMPT_RUNTIME}
            $<TARGET_FILE_DIR:nt_tests>)
endif()
target_compile_definitions(nt_tests PRIVATE
    NT_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

if(NT_SANITIZE)
    target_compile_options(nt_tests PRIVATE -fsanitize=address,undefined)
    target_link_options(nt_tests PRIVATE -fsanitize=address,undefined)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(nt_tests)
