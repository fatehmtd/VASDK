# --- Download Whistle STT Model + Needle Engine ---
#
# Whistle (Cactus Compute, Apache-2.0) is a 17 MB multilingual speech-to-text model
# (en, de, fr, es, it, nl, pl). It runs on Cactus's closed-source "needle" engine, which is
# published as prebuilt binaries only; there is no source to build. This module fetches:
#
#   ${WHISTLE_STT_MODEL_DIR}/whistle.cact        - model weights (Cactus-Compute/whistle)
#   ${WHISTLE_STT_MODEL_DIR}/<engine library>    - desktop only: the shared engine, loaded at runtime
#   ${WHISTLE_ENGINE_DIR}/include/needle.h       - C API header
#   ${WHISTLE_ENGINE_DIR}/libneedle.a            - Android only: static engine, linked in
#
# Why a shared library on desktop: the static libneedle.a for Windows/Linux/macOS is built
# with clang + libc++, which MSVC and GCC/libstdc++ builds cannot link. The engine shipped
# in the cactus-needle Python wheel is the same code as a self-contained shared library
# (libc++ linked in, only the C runtime imported) exporting the plain C API, so the
# provider loads it with LoadLibrary/dlopen. Android's libneedle.a is NDK-built
# (std::__ndk1) and links directly.
#
# Every file is pinned by HuggingFace revision and SHA-256.
#
# Usage:
#   cmake -DVASDK_BUILD_WHISTLE_STT=ON ..
#   cmake -DVASDK_DOWNLOAD_WHISTLE_STT_MODEL=OFF ..       # skip downloads
#   cmake -DWHISTLE_STT_MODEL_DIR=/path/to/models ..      # custom model location

option(VASDK_DOWNLOAD_WHISTLE_STT_MODEL
    "Download the Whistle STT model and needle engine from HuggingFace during configure" ON)

set(WHISTLE_STT_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/whistle"
    CACHE PATH "Directory to store whistle.cact and the needle engine library")

set(WHISTLE_ENGINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/needle-engine")

set(WHISTLE_MODEL_REVISION  "741118687e1f008f06d1351a402a731274789790")  # Cactus-Compute/whistle
set(WHISTLE_ENGINE_REVISION "d6c7d6a2e9655505e0f8e23339356dd1705900eb")  # Cactus-Compute/needle3
set(WHISTLE_ENGINE_VERSION  "3.3.3")                                     # cactus-needle wheel

set(_whistle_model_base  "https://huggingface.co/Cactus-Compute/whistle/resolve/${WHISTLE_MODEL_REVISION}")
set(_whistle_engine_base "https://huggingface.co/Cactus-Compute/needle3/resolve/${WHISTLE_ENGINE_REVISION}")

file(MAKE_DIRECTORY "${WHISTLE_STT_MODEL_DIR}" "${WHISTLE_ENGINE_DIR}/include")

# ---------------------------------------------------------------------------
# Pick the engine artifact for the target platform.
#   _whistle_engine_kind: "static" (libneedle.a) or "wheel" (shared library inside a wheel)
# ---------------------------------------------------------------------------
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _whistle_cpu)
set(_whistle_engine_kind "")
if(ANDROID)
    set(_whistle_engine_kind "static")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_whistle_engine_file "android-arm64/libneedle.a")
        set(_whistle_engine_sha  "7aa6de6132a22bf45f5e903425feb66f159ad46f47fa558921bc4fd02c3a2fa0")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_whistle_engine_file "android-armv7/libneedle.a")
        set(_whistle_engine_sha  "9f559a287b201c213e5c753d3cd1076a1849c73f94b445c85d11e4969422a135")
    else()
        set(_whistle_engine_kind "")
    endif()
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Only 64-bit Windows engines are published; a 32-bit process cannot load them
    # (LoadLibrary fails with error 193).
    message(FATAL_ERROR "[WhistleSTT] The needle engine is 64-bit only, but this build targets "
                        "32-bit Windows. Reconfigure with -A x64 (or -A ARM64), or set "
                        "VASDK_BUILD_WHISTLE_STT=OFF.")
elseif(WIN32)
    set(_whistle_engine_kind "wheel")
    set(_whistle_engine_lib  "libneedle3.dll")
    if(_whistle_cpu MATCHES "arm64|aarch64" OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
        set(_whistle_engine_file "win_arm64")
        set(_whistle_engine_sha  "92e4f0edc33f83cf2a67b878d74ec39ec2a2941f0fbbc7f5832c68301e81fb22")
    else()
        set(_whistle_engine_file "win_amd64")
        set(_whistle_engine_sha  "b6447b2be7426b7ff3b06360b15a90a8b7870e3fa2275023e20541d5e8d81bdb")
    endif()
elseif(APPLE)
    set(_whistle_engine_kind "wheel")
    set(_whistle_engine_lib  "libneedle3.dylib")
    if(_whistle_cpu MATCHES "arm64|aarch64")
        set(_whistle_engine_file "macosx_11_0_arm64")
        set(_whistle_engine_sha  "c5538e045fb26f0b6f911102516582d4434fbfa38aa28e5c2f3eb990b92aaf8d")
    else()
        set(_whistle_engine_file "macosx_11_0_x86_64")
        set(_whistle_engine_sha  "504b4e14dccff1d9a0591fcd78cc18063b99be396f6935a1c2b8189f0f8de89d")
    endif()
elseif(UNIX)
    set(_whistle_engine_kind "wheel")
    set(_whistle_engine_lib  "libneedle3.so")
    if(_whistle_cpu MATCHES "arm64|aarch64")
        set(_whistle_engine_file "manylinux2014_aarch64")
        set(_whistle_engine_sha  "cda9aa23eec3cc2cc6fea43ae5ba32b9d39a1487a198d8e2d86c1314dfe04873")
    elseif(_whistle_cpu MATCHES "x86_64|amd64")
        set(_whistle_engine_file "manylinux2014_x86_64")
        set(_whistle_engine_sha  "b909897e11cf4da9566df6370f8a63564693fbd6edeaac0b8678b71865d4dc24")
    else()
        set(_whistle_engine_kind "")
    endif()
endif()

if(_whistle_engine_kind STREQUAL "")
    message(WARNING "[WhistleSTT] No prebuilt needle engine for this platform "
                    "(${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}${ANDROID_ABI}).")
endif()

# ---------------------------------------------------------------------------
# Helper: download one file with a SHA-256 check, skip if already present.
# ---------------------------------------------------------------------------
function(whistle_download URL DEST SHA256 OUT_OK)
    set(${OUT_OK} TRUE PARENT_SCOPE)
    get_filename_component(_name "${DEST}" NAME)
    if(EXISTS "${DEST}")
        file(SHA256 "${DEST}" _have)
        if(_have STREQUAL "${SHA256}")
            message(STATUS "[WhistleSTT] ${_name} already present, skipping.")
            return()
        endif()
        message(STATUS "[WhistleSTT] ${_name} does not match the pinned hash, re-downloading.")
        file(REMOVE "${DEST}")
    endif()
    message(STATUS "[WhistleSTT] Downloading ${_name} ...")
    file(DOWNLOAD "${URL}" "${DEST}.part"
         EXPECTED_HASH SHA256=${SHA256}
         STATUS dl_status
         SHOW_PROGRESS
         TIMEOUT 600)
    list(GET dl_status 0 err_code)
    list(GET dl_status 1 err_str)
    if(NOT err_code EQUAL 0)
        message(WARNING "[WhistleSTT] Failed to download ${_name}: ${err_str}")
        file(REMOVE "${DEST}.part")
        set(${OUT_OK} FALSE PARENT_SCOPE)
    else()
        file(RENAME "${DEST}.part" "${DEST}")
        message(STATUS "[WhistleSTT] ${_name} saved to ${DEST}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
if(VASDK_DOWNLOAD_WHISTLE_STT_MODEL)
    whistle_download("${_whistle_model_base}/whistle.cact"
                     "${WHISTLE_STT_MODEL_DIR}/whistle.cact"
                     "b6e02f048568ac5d01a2042556c658061e699acbc0aa2a1439f52f3d461dffeb" _ok)

    # The header is identical in every platform folder.
    whistle_download("${_whistle_engine_base}/linux-x86_64/needle.h"
                     "${WHISTLE_ENGINE_DIR}/include/needle.h"
                     "a27dac4b3239eead91d124b7f7dbce5495c13019903fd817b68c16d39b055a99" _ok)

    if(_whistle_engine_kind STREQUAL "static")
        whistle_download("${_whistle_engine_base}/${_whistle_engine_file}"
                         "${WHISTLE_ENGINE_DIR}/libneedle.a"
                         "${_whistle_engine_sha}" _ok)
    elseif(_whistle_engine_kind STREQUAL "wheel")
        set(_wheel "cactus_needle-${WHISTLE_ENGINE_VERSION}-py3-none-${_whistle_engine_file}.whl")
        set(_wheel_path "${WHISTLE_ENGINE_DIR}/${_wheel}")
        whistle_download("${_whistle_engine_base}/python/${_wheel}" "${_wheel_path}"
                         "${_whistle_engine_sha}" _ok)
        if(_ok AND (NOT EXISTS "${WHISTLE_STT_MODEL_DIR}/${_whistle_engine_lib}"
                    OR "${_wheel_path}" IS_NEWER_THAN "${WHISTLE_STT_MODEL_DIR}/${_whistle_engine_lib}"))
            # A wheel is a zip; pull out only the engine library.
            set(_unpack "${WHISTLE_ENGINE_DIR}/wheel")
            file(REMOVE_RECURSE "${_unpack}")
            file(MAKE_DIRECTORY "${_unpack}")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E tar xf "${_wheel_path}" "needle/${_whistle_engine_lib}"
                WORKING_DIRECTORY "${_unpack}"
                RESULT_VARIABLE _rc)
            if(_rc EQUAL 0 AND EXISTS "${_unpack}/needle/${_whistle_engine_lib}")
                file(COPY "${_unpack}/needle/${_whistle_engine_lib}" DESTINATION "${WHISTLE_STT_MODEL_DIR}")
                message(STATUS "[WhistleSTT] Engine ${_whistle_engine_lib} extracted to ${WHISTLE_STT_MODEL_DIR}")
            else()
                message(WARNING "[WhistleSTT] Could not extract ${_whistle_engine_lib} from ${_wheel}")
            endif()
            file(REMOVE_RECURSE "${_unpack}")
        endif()
    endif()
else()
    message(STATUS "[WhistleSTT] Download disabled. Place whistle.cact (and on desktop the "
                   "engine library) in ${WHISTLE_STT_MODEL_DIR}/ and needle.h in "
                   "${WHISTLE_ENGINE_DIR}/include/")
endif()

set(WHISTLE_ENGINE_STATIC OFF)
if(_whistle_engine_kind STREQUAL "static")
    set(WHISTLE_ENGINE_STATIC ON)
endif()

# Expose locations to parent scope so the provider target can reference them.
set(WHISTLE_STT_MODEL_DIR "${WHISTLE_STT_MODEL_DIR}" PARENT_SCOPE)
set(WHISTLE_ENGINE_DIR    "${WHISTLE_ENGINE_DIR}"    PARENT_SCOPE)
set(WHISTLE_ENGINE_STATIC "${WHISTLE_ENGINE_STATIC}" PARENT_SCOPE)
