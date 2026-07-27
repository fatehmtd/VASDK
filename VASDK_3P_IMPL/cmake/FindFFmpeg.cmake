# 
# FindFFmpeg.cmake
#
# Locates the FFmpeg libraries required by FFmpegAudioSource:
#   libavformat  – container demuxing (MP3, FLAC, WAV, OGG, M4A, OPUS, …)
#   libavcodec   – audio decoder
#   libavutil    – shared utilities
#   libswresample – sample-rate/format conversion
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#   find_package(FFmpeg)          # optional
#   find_package(FFmpeg REQUIRED) # required
#   target_link_libraries(your_target PRIVATE FFmpeg::FFmpeg)
#
# Sets:
#   FFmpeg_FOUND         – TRUE if all four libraries were located
#   FFmpeg_INCLUDE_DIRS  – include paths
#   FFmpeg_LIBRARIES     – link libraries / flags
#
# Search order:
#   1. pkg-config  (Linux, macOS, and Windows vcpkg with CMAKE_PREFIX_PATH)
#   2. FFMPEG_DIR  CMake variable or environment variable (manual install)
#   3. Common default paths
#
# Quick-start:
#   Linux:   sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
#   macOS:   brew install ffmpeg
#   Windows: vcpkg install ffmpeg:x64-windows
#            – or –
#            Download a shared build from https://github.com/GyanD/codexffmpeg/releases
#            and set: cmake -DFFMPEG_DIR=C:/path/to/ffmpeg ..
# 

#  0. Bootstrap FFMPEG_DIR from the environment 
# Promote $ENV{FFMPEG_DIR} → CMake cache variable so it appears in cmake-gui /
# ccmake and so CMake does not silently ignore a newly-set env var on re-runs.
if(NOT DEFINED FFMPEG_DIR OR FFMPEG_DIR STREQUAL "")
    if(DEFINED ENV{FFMPEG_DIR})
        set(FFMPEG_DIR "$ENV{FFMPEG_DIR}" CACHE PATH
            "Root directory of the FFmpeg installation (initialised from FFMPEG_DIR env var)")
        message(STATUS "[FFmpeg] Using FFMPEG_DIR from environment: ${FFMPEG_DIR}")
    else()
        set(FFMPEG_DIR "" CACHE PATH
            "Root directory of the FFmpeg installation (set FFMPEG_DIR env var or -DFFMPEG_DIR=<path>)")
    endif()
endif()

# If FFMPEG_DIR is now set but previous find_path / find_library runs cached
# NOTFOUND, clear those stale entries so this run re-searches with the new root.
if(FFMPEG_DIR)
    foreach(_stale FFmpeg_INCLUDE_DIR
                   FFmpeg_AVFORMAT_LIB FFmpeg_AVCODEC_LIB
                   FFmpeg_AVUTIL_LIB   FFmpeg_SWRESAMPLE_LIB)
        if("${${_stale}}" MATCHES "NOTFOUND")
            unset(${_stale} CACHE)
        endif()
    endforeach()
endif()

#  1. Try pkg-config 
find_package(PkgConfig QUIET)

set(_FFmpeg_PKGCONFIG_OK FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_AVFORMAT    QUIET libavformat)
    pkg_check_modules(PC_AVCODEC     QUIET libavcodec)
    pkg_check_modules(PC_AVUTIL      QUIET libavutil)
    pkg_check_modules(PC_SWRESAMPLE  QUIET libswresample)

    if(PC_AVFORMAT_FOUND AND PC_AVCODEC_FOUND AND PC_AVUTIL_FOUND AND PC_SWRESAMPLE_FOUND)
        set(_FFmpeg_PKGCONFIG_OK TRUE)

        set(FFmpeg_INCLUDE_DIRS
            ${PC_AVFORMAT_INCLUDE_DIRS}
            ${PC_AVCODEC_INCLUDE_DIRS}
            ${PC_AVUTIL_INCLUDE_DIRS}
            ${PC_SWRESAMPLE_INCLUDE_DIRS}
        )
        list(REMOVE_DUPLICATES FFmpeg_INCLUDE_DIRS)

        set(FFmpeg_LIBRARIES
            ${PC_AVFORMAT_LINK_LIBRARIES}
            ${PC_AVCODEC_LINK_LIBRARIES}
            ${PC_AVUTIL_LINK_LIBRARIES}
            ${PC_SWRESAMPLE_LINK_LIBRARIES}
        )
        list(REMOVE_DUPLICATES FFmpeg_LIBRARIES)
    endif()
endif()

#  2. Manual search (Windows / custom layout) 
if(NOT _FFmpeg_PKGCONFIG_OK)
    set(_FFmpeg_HINTS
        "$ENV{FFMPEG_DIR}"
        "${FFMPEG_DIR}"
        "C:/ffmpeg"
        "C:/tools/ffmpeg"
        "${CMAKE_PREFIX_PATH}"
    )

    find_path(FFmpeg_INCLUDE_DIR libavformat/avformat.h
        HINTS    ${_FFmpeg_HINTS}
        PATH_SUFFIXES include
        DOC "FFmpeg include directory"
    )

    foreach(_comp avformat avcodec avutil swresample)
        string(TOUPPER ${_comp} _COMP_UP)
        find_library(FFmpeg_${_COMP_UP}_LIB ${_comp}
            HINTS    ${_FFmpeg_HINTS}
            PATH_SUFFIXES lib lib/x64 lib64
            DOC "FFmpeg lib${_comp}"
        )
    endforeach()

    if(FFmpeg_INCLUDE_DIR         AND
       FFmpeg_AVFORMAT_LIB        AND
       FFmpeg_AVCODEC_LIB         AND
       FFmpeg_AVUTIL_LIB          AND
       FFmpeg_SWRESAMPLE_LIB)
        set(FFmpeg_INCLUDE_DIRS "${FFmpeg_INCLUDE_DIR}")
        set(FFmpeg_LIBRARIES
            "${FFmpeg_AVFORMAT_LIB}"
            "${FFmpeg_AVCODEC_LIB}"
            "${FFmpeg_AVUTIL_LIB}"
            "${FFmpeg_SWRESAMPLE_LIB}"
        )
    endif()
endif()

#  3. Validate and report 
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_INCLUDE_DIRS FFmpeg_LIBRARIES
    FAIL_MESSAGE "FFmpeg not found. Install it and re-run CMake:\n  Linux:   sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev\n  macOS:   brew install ffmpeg\n  Windows: vcpkg install ffmpeg:x64-windows  OR  set FFMPEG_DIR env var / -DFFMPEG_DIR=<path>"
)

#  4. Create imported interface target 
if(FFmpeg_FOUND AND NOT TARGET FFmpeg::FFmpeg)
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    target_include_directories(FFmpeg::FFmpeg INTERFACE ${FFmpeg_INCLUDE_DIRS})
    target_link_libraries(FFmpeg::FFmpeg      INTERFACE ${FFmpeg_LIBRARIES})
endif()
