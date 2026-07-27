# FindONNXRuntime.cmake - External CMake module for ONNX Runtime
# Usage: include(path/to/FindONNXRuntime.cmake)

include(FetchContent)

# Guard to prevent multiple inclusions
if(TARGET onnxruntime_lib)
    return()
endif()

# Set ONNX Runtime version
set(ONNXRUNTIME_VERSION "1.19.2" CACHE STRING "ONNX Runtime version to download")


if(WIN32)
    set(ONNXRUNTIME_ARCHIVE_EXT "zip" CACHE STRING "ONNX Runtime Archive extension: zip for windows")
else()
    set(ONNXRUNTIME_ARCHIVE_EXT "tgz" CACHE STRING "ONNX Runtime Archive extension")
endif()

# Determine platform and architecture
if(WIN32)    
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ONNXRUNTIME_PLATFORM "win-x64")
        set(ONNXRUNTIME_LIB_NAME "onnxruntime.lib")
        set(ONNXRUNTIME_DLL_NAME "onnxruntime.dll")
    else()
        set(ONNXRUNTIME_PLATFORM "win-x86")
        set(ONNXRUNTIME_LIB_NAME "onnxruntime.lib")
        set(ONNXRUNTIME_DLL_NAME "onnxruntime.dll")
    endif()
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|ARM64")
        set(ONNXRUNTIME_PLATFORM "osx-arm64")
    else()
        set(ONNXRUNTIME_PLATFORM "osx-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.dylib")
elseif(ANDROID)
    # ANDROID_ABI is set by the NDK toolchain (arm64-v8a, armeabi-v7a, x86_64, x86)
    if(NOT ANDROID_ABI)
        message(FATAL_ERROR "ANDROID_ABI is not set - ensure the Android NDK toolchain is active")
    endif()
    set(ONNXRUNTIME_PLATFORM "android")
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(ONNXRUNTIME_PLATFORM "linux-aarch64")
    else()
        set(ONNXRUNTIME_PLATFORM "linux-x64")
    endif()
    set(ONNXRUNTIME_LIB_NAME "libonnxruntime.so")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# Construct download URL
# Android uses an AAR package (a ZIP containing multiple ABI flavours)
# All other platforms use the normal zip/tgz prebuilt archives
if(ANDROID)
    set(ONNXRUNTIME_URL
        "https://repo1.maven.org/maven2/com/microsoft/onnxruntime/onnxruntime-${ONNXRUNTIME_PLATFORM}/${ONNXRUNTIME_VERSION}/onnxruntime-${ONNXRUNTIME_PLATFORM}-${ONNXRUNTIME_VERSION}.aar"
    )
else()
    set(ONNXRUNTIME_URL
        "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-${ONNXRUNTIME_PLATFORM}-${ONNXRUNTIME_VERSION}.${ONNXRUNTIME_ARCHIVE_EXT}"
    )
endif()

message(STATUS "Downloading ONNX Runtime for ${ONNXRUNTIME_PLATFORM}")
message(STATUS "URL: ${ONNXRUNTIME_URL}")

# Download and extract ONNX Runtime
message(STATUS "Downloading without hash verification for ${ONNXRUNTIME_PLATFORM}")
FetchContent_Declare(
    onnxruntime
    URL "${ONNXRUNTIME_URL}"
)

FetchContent_MakeAvailable(onnxruntime)

# Create ONNX Runtime interface library
add_library(onnxruntime_lib INTERFACE)

# Android AAR layout: headers/ contains the C/C++ headers,
# jni/${ANDROID_ABI}/ contains the shared library.
if(ANDROID)
    set(ONNXRUNTIME_INCLUDE_DIR "${onnxruntime_SOURCE_DIR}/headers")
    set(ONNXRUNTIME_LIB_PATH    "${onnxruntime_SOURCE_DIR}/jni/${ANDROID_ABI}/${ONNXRUNTIME_LIB_NAME}")

    if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
        message(FATAL_ERROR "ONNX Runtime Android AAR headers not found at ${ONNXRUNTIME_INCLUDE_DIR}. "
                            "Check that v${ONNXRUNTIME_VERSION} publishes an AAR with a headers/ directory.")
    endif()
    if(NOT EXISTS "${ONNXRUNTIME_LIB_PATH}")
        message(FATAL_ERROR "ONNX Runtime Android library not found at ${ONNXRUNTIME_LIB_PATH}. "
                            "ABI '${ANDROID_ABI}' may not be present in the AAR.")
    endif()

    target_include_directories(onnxruntime_lib INTERFACE ${ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(onnxruntime_lib INTERFACE ${ONNXRUNTIME_LIB_PATH})
    message(STATUS "Using Android AAR include path: ${ONNXRUNTIME_INCLUDE_DIR}")
    message(STATUS "Using Android AAR library: ${ONNXRUNTIME_LIB_PATH}")
else()
    # Set include directories - ONNX Runtime packages typically have include/ or include/onnxruntime/
    # We'll try both common paths
    if(EXISTS "${onnxruntime_SOURCE_DIR}/include/onnxruntime_cxx_api.h")
        # Direct include path
        target_include_directories(onnxruntime_lib INTERFACE 
            ${onnxruntime_SOURCE_DIR}/include
        )
        message(STATUS "Using direct include path: ${onnxruntime_SOURCE_DIR}/include")
    elseif(EXISTS "${onnxruntime_SOURCE_DIR}/include/onnxruntime/core/session/onnxruntime_cxx_api.h")
        # Nested include path
        target_include_directories(onnxruntime_lib INTERFACE 
            ${onnxruntime_SOURCE_DIR}/include/onnxruntime/core/session
        )
        message(STATUS "Using nested include path: ${onnxruntime_SOURCE_DIR}/include/onnxruntime/core/session")
    else()
        # Fallback - search for the header file
        file(GLOB_RECURSE ONNX_HEADER_FILE "${onnxruntime_SOURCE_DIR}/*/onnxruntime_cxx_api.h")
        if(ONNX_HEADER_FILE)
            get_filename_component(ONNX_INCLUDE_DIR "${ONNX_HEADER_FILE}" DIRECTORY)
            target_include_directories(onnxruntime_lib INTERFACE ${ONNX_INCLUDE_DIR})
            message(STATUS "Found header at: ${ONNX_INCLUDE_DIR}")
        else()
            message(FATAL_ERROR "Could not find onnxruntime_cxx_api.h in ${onnxruntime_SOURCE_DIR}")
        endif()
    endif()

    # Set library path
    set(ONNXRUNTIME_LIB_PATH "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_LIB_NAME}")

    # Link libraries based on platform
    if(WIN32)
        # Windows: Link import library and copy DLL
        target_link_libraries(onnxruntime_lib INTERFACE ${ONNXRUNTIME_LIB_PATH})
        
        # Function to copy DLL to output directory
        function(copy_onnxruntime_dll target_name)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${onnxruntime_SOURCE_DIR}/lib/${ONNXRUNTIME_DLL_NAME}"
                $<TARGET_FILE_DIR:${target_name}>
            )
        endfunction()
    else()
        # Unix-like systems: Link shared library directly
        target_link_libraries(onnxruntime_lib INTERFACE ${ONNXRUNTIME_LIB_PATH})
        
        # Set RPATH for runtime loading (optional)
        set_target_properties(onnxruntime_lib PROPERTIES
            INTERFACE_LINK_OPTIONS "-Wl,-rpath,${onnxruntime_SOURCE_DIR}/lib"
        )
    endif()
endif() # end if(ANDROID)

# Optional: Print configuration info
message(STATUS "ONNX Runtime configuration:")
message(STATUS "  Platform: ${ONNXRUNTIME_PLATFORM}")
message(STATUS "  Library: ${ONNXRUNTIME_LIB_NAME}")
message(STATUS "  Source dir: ${onnxruntime_SOURCE_DIR}")
if(ANDROID)
    message(STATUS "  ABI: ${ANDROID_ABI}")
endif()
message(STATUS "  Library path: ${ONNXRUNTIME_LIB_PATH}")

# Debug function to explore directory structure
function(debug_onnxruntime_structure)
    message(STATUS "ONNX Runtime directory contents:")
    if(EXISTS "${onnxruntime_SOURCE_DIR}")
        file(GLOB CONTENTS "${onnxruntime_SOURCE_DIR}/*")
        foreach(ITEM ${CONTENTS})
            if(IS_DIRECTORY ${ITEM})
                get_filename_component(DIR_NAME ${ITEM} NAME)
                message(STATUS "  Directory: ${DIR_NAME}/")
                
                # Check for common header locations
                if(DIR_NAME STREQUAL "include")
                    file(GLOB_RECURSE HEADERS "${ITEM}/*.h")
                    foreach(HEADER ${HEADERS})
                        get_filename_component(HEADER_NAME ${HEADER} NAME)
                        if(HEADER_NAME STREQUAL "onnxruntime_cxx_api.h")
                            message(STATUS "    Found header: ${HEADER}")
                        endif()
                    endforeach()
                endif()
            else()
                get_filename_component(FILE_NAME ${ITEM} NAME)
                message(STATUS "  File: ${FILE_NAME}")
            endif()
        endforeach()
    endif()
endfunction()

# Call debug function if CMake is run with -DDEBUG_ONNXRUNTIME=ON
if(DEBUG_ONNXRUNTIME)
    debug_onnxruntime_structure()
endif()
