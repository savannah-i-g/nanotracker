# Locates libopenmpt and exposes it as libopenmpt::libopenmpt.
# libopenmpt ships no upstream CMake package. Resolution order:
#   1. the vendored static build under third_party/libopenmpt-install
#      (produced by tools/build_libopenmpt.sh)
#   2. a system package via pkg-config
set(_nt_openmpt_prefix "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libopenmpt-install")

if(EXISTS "${_nt_openmpt_prefix}/lib/libopenmpt.a")
    add_library(libopenmpt::libopenmpt STATIC IMPORTED)
    set_target_properties(libopenmpt::libopenmpt PROPERTIES
        IMPORTED_LOCATION "${_nt_openmpt_prefix}/lib/libopenmpt.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_nt_openmpt_prefix}/include")
    # The static archive needs zlib (module container decompression).
    find_package(ZLIB REQUIRED)
    set_property(TARGET libopenmpt::libopenmpt APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
    set(libopenmpt_FOUND TRUE)
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(NT_LIBOPENMPT QUIET IMPORTED_TARGET libopenmpt)
    endif()
    if(NT_LIBOPENMPT_FOUND AND NOT TARGET libopenmpt::libopenmpt)
        add_library(libopenmpt::libopenmpt ALIAS PkgConfig::NT_LIBOPENMPT)
        set(libopenmpt_FOUND TRUE)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libopenmpt REQUIRED_VARS libopenmpt_FOUND)
