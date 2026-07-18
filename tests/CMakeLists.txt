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
    fx_chain_test.cpp
    graph_test.cpp
    ntp_test.cpp
    clap_host_test.cpp
    midi_test.cpp
    midi_record_test.cpp
    ftrk_writer_test.cpp
    export_test.cpp
    export_suite_test.cpp
    export_presets_test.cpp
    local_api_test.cpp
    ${CMAKE_SOURCE_DIR}/src/api/local_api.cpp
    ${CMAKE_SOURCE_DIR}/src/app/midi_record.cpp
    ${CMAKE_SOURCE_DIR}/src/app/pattern_ops.cpp
    ${CMAKE_SOURCE_DIR}/src/app/project_session.cpp
    ${CMAKE_SOURCE_DIR}/src/app/undo.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/audio_device_openal.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/audio_engine.cpp
    ${CMAKE_SOURCE_DIR}/src/audio/decoders.cpp
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
    ${CMAKE_SOURCE_DIR}/src/ext/clap_host.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/editor_window.cpp
    ${CMAKE_SOURCE_DIR}/src/ext/vst3_host.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ftrk_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/io/ftrk_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_post.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_presets.cpp
    ${CMAKE_SOURCE_DIR}/src/io/export_render.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/import_common.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/it_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/mod_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/s3m_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/io/import/xm_importer.cpp
    ${CMAKE_SOURCE_DIR}/src/rt/rt_assert.cpp)

target_include_directories(nt_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(nt_tests PRIVATE Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json OpenAL::OpenAL dr_libs stb samplerate
    libopenmpt::libopenmpt miniz clap vst3_hosting rtmidi ixwebsocket
    vorbisenc vorbis ogg ebur128
    ${CMAKE_DL_LIBS} $<$<BOOL:${UNIX}>:X11::X11>)

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
