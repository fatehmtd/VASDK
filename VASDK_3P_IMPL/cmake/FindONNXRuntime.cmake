# ONNX Runtime, for the providers that run ONNX models: the target onnxruntime_lib.
#
# An installed ONNX Runtime (Conan, a distro: find_package(onnxruntime)) when
# VASDK_DEPENDENCY_SOURCE is SYSTEM or AUTO, else Microsoft's prebuilt build (the NuGet package on
# desktop, the AAR on Android), pinned by SHA-256. The prebuilt one is an imported shared library,
# so CMake sets the build tree's rpath, Windows finds the DLL through $<TARGET_RUNTIME_DLLS>, and the
# C library's install puts it beside itself (cmake/VASDKInstall.cmake).
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

#  Microsoft's prebuilt ONNX Runtime, pinned by SHA-256 (version 1.19.2). Android: the AAR on Maven
#  Central. Desktop: the NuGet package Microsoft.ML.OnnxRuntime, which holds every platform; the
#  GitHub release archives went away with github.com/microsoft/onnxruntime.
if(ANDROID)
    # ANDROID_ABI is set by the NDK toolchain (arm64-v8a, armeabi-v7a, x86_64, x86)
    if(NOT ANDROID_ABI)
        message(FATAL_ERROR "ANDROID_ABI is not set - ensure the Android NDK toolchain is active")
    endif()
    set(ONNXRUNTIME_PLATFORM "android")
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
    set(ONNXRUNTIME_URL "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-android/${ONNXRUNTIME_VERSION}/onnxruntime-android-${ONNXRUNTIME_VERSION}.aar")
    set(_ort_hash 84737dd1fdb715f0ee695c0a9864f6949b66ecb4c46ca733bc9211bf0c2a7b5f)
else()
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ONNXRUNTIME_PLATFORM "win-x64")
        else()
            set(ONNXRUNTIME_PLATFORM "win-x86")
        endif()
        set(ONNXRUNTIME_LIB_NAME "onnxruntime.dll")
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64")
            set(ONNXRUNTIME_PLATFORM "osx-arm64")
        else()
            set(ONNXRUNTIME_PLATFORM "osx-x64")
        endif()
        # The name the library's install name gives (@rpath/libonnxruntime.1.19.2.dylib)
        set(ONNXRUNTIME_LIB_NAME "libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            set(ONNXRUNTIME_PLATFORM "linux-arm64")
        else()
            set(ONNXRUNTIME_PLATFORM "linux-x64")
        endif()
        set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so.${ONNXRUNTIME_VERSION}")
    else()
        message(FATAL_ERROR "ONNX Runtime: no prebuilt release for ${CMAKE_SYSTEM_NAME}")
    endif()
    set(ONNXRUNTIME_URL "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${ONNXRUNTIME_VERSION}")
    set(_ort_hash fa133c8fe6238445a0dd3e8447baa5c7d16f418caa8e576c7e253ba77b27b61b)
endif()

# The hashes are those of 1.19.2: another version (ONNXRUNTIME_VERSION) is downloaded unchecked.
set(_ort_url_hash)
if(ONNXRUNTIME_VERSION STREQUAL "1.19.2")
    set(_ort_url_hash URL_HASH SHA256=${_ort_hash})
else()
    message(WARNING "ONNX Runtime ${ONNXRUNTIME_VERSION} for ${ONNXRUNTIME_PLATFORM}: no pinned SHA-256, downloaded unchecked")
endif()

message(STATUS "ONNX Runtime ${ONNXRUNTIME_VERSION} for ${ONNXRUNTIME_PLATFORM}: ${ONNXRUNTIME_URL}")
if(ANDROID)
    FetchContent_Declare(onnxruntime URL "${ONNXRUNTIME_URL}" ${_ort_url_hash})
    FetchContent_MakeAvailable(onnxruntime)
else()
    # The package is a zip named .nupkg, which FetchContent won't unpack: download it as is, then lay
    # out this platform's files as the old release archives did (include/, lib/ under the names the
    # libraries load by), so everything that uses onnxruntime_SOURCE_DIR stays the same.
    FetchContent_Declare(onnxruntime_nupkg URL "${ONNXRUNTIME_URL}" ${_ort_url_hash} DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(onnxruntime_nupkg)
    set(onnxruntime_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/onnxruntime-${ONNXRUNTIME_VERSION}-${ONNXRUNTIME_PLATFORM}")
    if(NOT EXISTS "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIB_NAME}")
        file(GLOB _ort_nupkg "${onnxruntime_nupkg_SOURCE_DIR}/*")
        set(_ort_staging "${onnxruntime_SOURCE_DIR}-staging")
        file(REMOVE_RECURSE "${_ort_staging}" "${onnxruntime_SOURCE_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${_ort_nupkg}" DESTINATION "${_ort_staging}"
             PATTERNS "build/native/include/*" "runtimes/${ONNXRUNTIME_PLATFORM}/native/*" LICENSE ThirdPartyNotices.txt)
        file(COPY "${_ort_staging}/build/native/include/" DESTINATION "${onnxruntime_SOURCE_DIR}/include")
        file(COPY "${_ort_staging}/LICENSE" "${_ort_staging}/ThirdPartyNotices.txt" DESTINATION "${onnxruntime_SOURCE_DIR}")
        set(_ort_native "${_ort_staging}/runtimes/${ONNXRUNTIME_PLATFORM}/native")
        file(MAKE_DIRECTORY "${onnxruntime_SOURCE_DIR}/lib")
        if(WIN32)
            file(COPY "${_ort_native}/" DESTINATION "${onnxruntime_SOURCE_DIR}/lib")
        elseif(APPLE)
            file(COPY_FILE "${_ort_native}/libonnxruntime.dylib" "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIB_NAME}")
            file(CREATE_LINK "${ONNXRUNTIME_LIB_NAME}" "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib" SYMBOLIC)
        else()   # soname libonnxruntime.so.1
            file(COPY_FILE "${_ort_native}/libonnxruntime.so" "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIB_NAME}")
            file(CREATE_LINK "${ONNXRUNTIME_LIB_NAME}" "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so.1" SYMBOLIC)
            file(CREATE_LINK "libonnxruntime.so.1" "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so" SYMBOLIC)
            file(COPY "${_ort_native}/libonnxruntime_providers_shared.so" DESTINATION "${onnxruntime_SOURCE_DIR}/lib")
        endif()
        file(REMOVE_RECURSE "${_ort_staging}")
    endif()
endif()

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
