# The C library: vasdk_c, VASDK::c (docs/integrating.md). One shared library holding the C API
# (VASDK/c/vasdk.h) and every provider this build has (VASDK/c/vasdk_providers.h), with their
# dependencies (protobuf, abseil, spdlog, curl...) linked in and hidden: it exports the vasdk_*
# functions and nothing else. A stable binary interface across releases of one major version
# (VASDK_C_API_VERSION), so it is what the bindings load (Python, Kotlin, Android) and what the
# release archives and the packages ship.
#
#   libvasdk_c.so.1 (Linux, Android)   libvasdk_c.1.dylib (macOS)   vasdk_c.dll + vasdk_c.lib (Windows)
#
# Built from the static C++ library: VASDK_SHARED turns it off (the C API is then VASDK's own).

if(NOT VASDK_ENABLE_ENGINE_CONFIG)
    message(WARNING "VASDK_BUILD_C_LIBRARY: the C API needs VASDK_ENABLE_ENGINE_CONFIG: vasdk_c is not built")
    return()
endif()
get_target_property(_vasdk_type VASDK TYPE)
if(NOT _vasdk_type STREQUAL "STATIC_LIBRARY")
    message(STATUS "VASDK: VASDK is a shared library (VASDK_SHARED), which exports the C API itself: vasdk_c is not built")
    return()
endif()

# The C API version is the soname: libvasdk_c.so.1
file(STRINGS ${VASDK_ROOT_DIR}/VASDK/include/VASDK/c/vasdk.h _vasdk_c_api_line REGEX "^#define VASDK_C_API_VERSION [0-9]+")
string(REGEX REPLACE "^#define VASDK_C_API_VERSION ([0-9]+).*" "\\1" VASDK_C_API_VERSION "${_vasdk_c_api_line}")
set(VASDK_C_API_VERSION ${VASDK_C_API_VERSION} PARENT_SCOPE)   # find_package(VASDK) reports it too

if(TARGET vasdk_provider_catalog)
    # vasdk_providers_add_all and vasdk_providers_set_log_level, exported
    add_library(vasdk_c SHARED ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/ProviderCatalog/ProvidersShared.cpp)
    target_link_libraries(vasdk_c PRIVATE vasdk_provider_catalog)
else()
    # Without the third-party providers (VASDK_BUILD_THIRDPARTY_IMPL off): the same two functions,
    # with no provider to add
    add_library(vasdk_c SHARED ${VASDK_ROOT_DIR}/VASDK_3P_IMPL/ProviderCatalog/ProvidersShared.cpp NoProviders.cpp)
endif()
add_library(VASDK::c ALIAS vasdk_c)

# VASDK whole: every C API function, not only those something here calls. The catalog links VASDK
# too, so the override applies the feature wherever it comes up in the link. With the catalog,
# VASDK comes through it alone: linked here as well, CMake would place it twice (here, and again
# after the provider libraries that need it), and GNU ld would take both copies whole.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    if(NOT TARGET vasdk_provider_catalog)
        target_link_libraries(vasdk_c PRIVATE VASDK)
    endif()
    set_property(TARGET vasdk_c PROPERTY LINK_LIBRARY_OVERRIDE_VASDK WHOLE_ARCHIVE)
elseif(MSVC)
    target_link_libraries(vasdk_c PRIVATE VASDK)
    target_link_options(vasdk_c PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:VASDK>")
elseif(APPLE)
    target_link_libraries(vasdk_c PRIVATE VASDK)
    target_link_options(vasdk_c PRIVATE "LINKER:-force_load,$<TARGET_FILE:VASDK>")
else()
    target_link_libraries(vasdk_c PRIVATE "-Wl,--whole-archive" VASDK "-Wl,--no-whole-archive")
endif()

target_include_directories(vasdk_c INTERFACE
    $<BUILD_INTERFACE:${VASDK_ROOT_DIR}/VASDK/include>
    $<BUILD_INTERFACE:${VASDK_ROOT_BINARY_DIR}/VASDK/include>   # version.h
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(vasdk_c PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${VASDK_C_API_VERSION}
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    EXPORT_NAME c)
# Found beside itself once installed: ONNX Runtime and the Whistle engine are installed next to it
if(APPLE)
    set_target_properties(vasdk_c PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(vasdk_c PROPERTIES INSTALL_RPATH "\$ORIGIN")
endif()

# Nothing but vasdk_*: the static libraries inside (protobuf, abseil, spdlog, the providers) keep
# their symbols to themselves, so they can't clash with another copy in the program.
if(APPLE)
    target_link_options(vasdk_c PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/vasdk_c.exported_symbols")
    set_property(TARGET vasdk_c APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vasdk_c.exported_symbols)
elseif(UNIX)   # Linux, Android
    target_link_options(vasdk_c PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vasdk_c.map")
    set_property(TARGET vasdk_c APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vasdk_c.map)
endif()
# Windows exports only what is marked: VASDK_C_API (dllexport while VASDK is compiled) and the
# two functions of ProvidersShared.cpp.

# Windows has no rpath: the DLLs it loads (ONNX Runtime, OpenSSL, an installed protobuf or curl) beside it in
# the build tree, so the bindings' tests and vasdk_copy_runtime() find them there
include(${CMAKE_CURRENT_SOURCE_DIR}/VASDKCopyRuntime.cmake)
if(WIN32)
    get_property(_vasdk_copy_script GLOBAL PROPERTY VASDK_COPY_RUNTIME_SCRIPT)
    add_custom_command(TARGET vasdk_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DVASDK_COPY_FILES=$<TARGET_RUNTIME_DLLS:vasdk_c>;${VASDK_OPENSSL_RUNTIME_FILES}"
                "-DVASDK_COPY_TO=$<TARGET_FILE_DIR:vasdk_c>"
                "-DVASDK_COPY_LOCK=${CMAKE_BINARY_DIR}/CMakeFiles/vasdk_copy_runtime.lock" -P "${_vasdk_copy_script}"
        VERBATIM)
endif()

# vasdk: the command installed with the library (models list|pull|verify|remove, version), a
# program of the C library's like any other: VASDK/c/vasdk.hpp over VASDK::c. Beside vasdk_c, so
# it finds it in the build tree and once installed.
if(NOT ANDROID)
    add_executable(vasdk_command ${VASDK_ROOT_DIR}/VASDK/tools/vasdk/main.cpp)
    target_link_libraries(vasdk_command PRIVATE vasdk_c nlohmann_json::nlohmann_json)
    target_compile_features(vasdk_command PRIVATE cxx_std_17)
    set_target_properties(vasdk_command PROPERTIES OUTPUT_NAME vasdk)
    if(APPLE)
        set_target_properties(vasdk_command PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set_target_properties(vasdk_command PROPERTIES INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(VASDK_BUILD_TESTS)
        add_test(NAME vasdk_command
                 COMMAND ${CMAKE_COMMAND} -DVASDK_COMMAND=$<TARGET_FILE:vasdk_command>
                         -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/vasdk_command_test -P ${CMAKE_CURRENT_SOURCE_DIR}/test_vasdk_command.cmake)
    endif()
endif()

# The C library's headers, its runtime dependencies, the CMake package and pkg-config file are
# installed by cmake/VASDKInstall.cmake.
