# ONNX Runtime, for the providers that run ONNX models: the target onnxruntime_lib.
#
# An installed ONNX Runtime (Conan, a distro: find_package(onnxruntime)) when
# VASDK_DEPENDENCY_SOURCE is SYSTEM or AUTO, else Microsoft's prebuilt release, pinned by SHA-256.
# The prebuilt one is an imported shared library, so CMake sets the build tree's rpath, Windows
# finds the DLL through $<TARGET_RUNTIME_DLLS>, and the C library's install puts it beside itself
# (cmake/VASDKInstall.cmake).
#
# Usage: include(path/to/FindONNXRuntime.cmake)

include(FetchContent)

if(TARGET onnxruntime_lib)
    return()
endif()

set(ONNXRUNTIME_VERSION "1.19.2" CACHE STRING "ONNX Runtime version to download")

#  An installed ONNX Runtime
if(DEFINED VASDK_DEPENDENCY_SOURCE AND NOT VASDK_DEPENDENCY_SOURCE STREQUAL "FETCH" AND NOT ANDROID)
    find_package(onnxruntime CONFIG QUIET)
    if(TARGET onnxruntime::onnxruntime)
        add_library(onnxruntime_lib INTERFACE)
        target_link_libraries(onnxruntime_lib INTERFACE onnxruntime::onnxruntime)
        set(VASDK_ONNXRUNTIME_INSTALLED ON)
        message(STATUS "VASDK: ONNX Runtime from the system (onnxruntime::onnxruntime ${onnxruntime_VERSION})")
        return()
    elseif(VASDK_DEPENDENCY_SOURCE STREQUAL "SYSTEM")
        message(FATAL_ERROR "VASDK: ONNX Runtime not found and VASDK_DEPENDENCY_SOURCE is SYSTEM: "
                            "install onnxruntime (Conan, your package manager) or use FETCH or AUTO")
    endif()
endif()

#  Microsoft's prebuilt release: platform, archive and its SHA-256 (version 1.19.2)
set(_ort_hash "")
if(ANDROID)
    # ANDROID_ABI is set by the NDK toolchain (arm64-v8a, armeabi-v7a, x86_64, x86)
    if(NOT ANDROID_ABI)
        message(FATAL_ERROR "ANDROID_ABI is not set - ensure the Android NDK toolchain is active")
    endif()
    set(ONNXRUNTIME_PLATFORM "android")
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
    set(ONNXRUNTIME_URL "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/${ONNXRUNTIME_VERSION}/onnxruntime-android-${ONNXRUNTIME_VERSION}.aar")
    set(_ort_hash 84737dd1fdb715f0ee695c0a9864f6949b66ecb4c46ca733bc9211bf0c2a7b5f)
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ONNXRUNTIME_PLATFORM "win-x64")
        set(_ort_hash dc4f841e511977c0a4f02e5066c3d9a58427644010ab4f89b918614a1cd4c2b0)
    else()
        set(ONNXRUNTIME_PLATFORM "win-x86")
    endif()
    set(ONNXRUNTIME_LIB_NAME "onnxruntime.dll")
    set(_ort_archive zip)
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64")
        set(ONNXRUNTIME_PLATFORM "osx-arm64")
        set(_ort_hash 370c49770e2e1f243e17c7b227bb7f4b3da793b847d02f38016dc0e46c30fbe1)
    else()
        set(ONNXRUNTIME_PLATFORM "osx-x86_64")
        set(_ort_hash 6536e36d7ea92e32d53dad7ddd0fdf10be5b62d1dace85a13e1295ff81e9b5d4)
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")
    set(_ort_archive tgz)
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
        set(ONNXRUNTIME_PLATFORM "linux-aarch64")
        set(_ort_hash 5e30145277d6d6fcb0e8f14f0d0ab5048af7b13ffd608023bb1e2875621fab07)
    else()
        set(ONNXRUNTIME_PLATFORM "linux-x64")
        set(_ort_hash eb00c64e0041f719913c4080e0fed7d9963dc3aa9b54664df6036d8308dbcd33)
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so.${ONNXRUNTIME_VERSION}")
    set(_ort_archive tgz)
else()
    message(FATAL_ERROR "ONNX Runtime: no prebuilt release for ${CMAKE_SYSTEM_NAME}")
endif()
if(NOT ANDROID)
    set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-${ONNXRUNTIME_PLATFORM}-${ONNXRUNTIME_VERSION}.${_ort_archive}")
endif()

# The hashes are those of 1.19.2: another version (ONNXRUNTIME_VERSION) is downloaded unchecked.
set(_ort_url_hash)
if(_ort_hash AND ONNXRUNTIME_VERSION STREQUAL "1.19.2")
    set(_ort_url_hash URL_HASH SHA256=${_ort_hash})
else()
    message(WARNING "ONNX Runtime ${ONNXRUNTIME_VERSION} for ${ONNXRUNTIME_PLATFORM}: no pinned SHA-256, downloaded unchecked")
endif()

message(STATUS "ONNX Runtime ${ONNXRUNTIME_VERSION} for ${ONNXRUNTIME_PLATFORM}: ${ONNXRUNTIME_URL}")
FetchContent_Declare(onnxruntime URL "${ONNXRUNTIME_URL}" ${_ort_url_hash})
FetchContent_MakeAvailable(onnxruntime)

# Android AAR: headers/ and jni/<abi>/; the desktop archives: include/ and lib/
if(ANDROID)
    set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_SOURCE_DIR}/headers")
    set(ONNXRUNTIME_LIB_PATH    "${onnxruntime_SOURCE_DIR}/jni/${ANDROID_ABI}/${ONNXRUNTIME_LIB_NAME}")
else()
    set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_SOURCE_DIR}/include")
    set(ONNXRUNTIME_LIB_PATH    "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIB_NAME}")
endif()
if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "ONNX Runtime: onnxruntime_cxx_api.h not found in ${ONNXRUNTIME_INCLUDE_DIR}")
endif()
if(NOT EXISTS "${ONNXRUNTIME_LIB_PATH}")
    message(FATAL_ERROR "ONNX Runtime: ${ONNXRUNTIME_LIB_PATH} not found (ABI or platform missing from the archive?)")
endif()

add_library(onnxruntime_lib SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime_lib PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_LIB_PATH}"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")
if(WIN32)
    set_target_properties(onnxruntime_lib PROPERTIES IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib")
endif()

# The files a program loads at run time, installed beside the C library
if(ANDROID)
    set(VASDK_ONNXRUNTIME_RUNTIME_FILES "${ONNXRUNTIME_LIB_PATH}")
elseif(WIN32)
    file(GLOB VASDK_ONNXRUNTIME_RUNTIME_FILES "${onnxruntime_SOURCE_DIR}/lib/*.dll")
elseif(APPLE)
    file(GLOB VASDK_ONNXRUNTIME_RUNTIME_FILES LIST_DIRECTORIES false "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime*.dylib")
else()   # the library and its soname links: libonnxruntime.so.1 -> libonnxruntime.so.1.19.2
    file(GLOB VASDK_ONNXRUNTIME_RUNTIME_FILES LIST_DIRECTORIES false "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime*.so*")
endif()
set(VASDK_ONNXRUNTIME_RUNTIME_FILES "${VASDK_ONNXRUNTIME_RUNTIME_FILES}" CACHE INTERNAL "")
