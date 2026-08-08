#  Build Options 

# Allow FetchContent_Populate(<name>) with declared details (CMake 4.x CMP0169)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Audio 
option(VASDK_BUILD_FFMPEG_AUDIO "Build FFmpeg audio file provider" OFF)
option(VASDK_BUILD_MINIAUDIO "Build MiniAudio audio I/O provider" ON)
option(VASDK_BUILD_SDL "Build SDL audio I/O provider" OFF)

# VAD
option(VASDK_BUILD_SILERO_VAD "Build Silero VAD provider" ON)
option(VASDK_BUILD_ENERGY_ZCR_VAD "Build Energy/ZCR VAD provider" OFF)

# Turn detection
option(VASDK_BUILD_SMART_TURN_V3 "Build SmartTurn v3 provider" ON)

# Transcription
option(VASDK_BUILD_GLADIAPP "Build Gladiapp transcription provider" OFF)
option(VASDK_BUILD_SONIOXPP_STT "Build SonioxPP transcription provider" OFF)
option(VASDK_BUILD_DEEPGRAMPP_STT "Build Deepgrampp transcription provider" OFF)
option(VASDK_BUILD_CARTESIAPP_STT "Build CartesiaPP transcription provider" OFF)
option(VASDK_BUILD_FISHAUDIOPP_STT "Build FishAudioPP transcription provider" OFF)
option(VASDK_BUILD_GRADIUMPP_STT "Build GradiumPP transcription provider" ON)
option(VASDK_BUILD_PARAKEET "Build Parakeet transcription provider" ON)
option(VASDK_BUILD_MOONSHINE_STT "Build Moonshine STT provider" OFF)
option(VASDK_BUILD_WHISPERCPP "Build WhisperCpp transcription provider" OFF)

# Voice Synthesis
option(VASDK_BUILD_DEEPGRAMPP_TTS "Build Deepgrampp voice synthesis provider" OFF)
option(VASDK_BUILD_CARTESIAPP_TTS "Build CartesiaPP voice synthesis provider" OFF)
option(VASDK_BUILD_FISHAUDIOPP_TTS "Build FishAudioPP voice synthesis provider" OFF)
option(VASDK_BUILD_GRADIUMPP_TTS "Build GradiumPP voice synthesis provider" ON)
option(VASDK_BUILD_SONIOXPP_TTS "Build SonioxPP voice synthesis provider" OFF)
option(VASDK_BUILD_SUPERTONIC_TTS "Build Supertonic TTS provider" OFF)
option(VASDK_BUILD_POCKET_TTS "Build Pocket TTS provider (flow-matching ONNX TTS at 24 kHz)" ON)

# Audio processing
option(VASDK_BUILD_RNNOISE "Build RNNoise noise-suppression DSP provider" ON)
option(VASDK_BUILD_VIZANIM "Build VizAnim audio features for live animations" ON)

# LLM inference
option(VASDK_BUILD_LLAMACPP "Build llama.cpp local LLM provider (offline GGUF inference)" ON)
option(VASDK_DOWNLOAD_LLAMACPP_MODEL "Download the GGUF model file for llama.cpp during configure" ON)

# NNAPI execution provider (Android only)
if(ANDROID)
    option(VASDK_USE_NNAPI "Enable NNAPI execution provider for ONNX Runtime on Android" OFF)
else()
    option(VASDK_USE_NNAPI "Enable NNAPI execution provider for ONNX Runtime on Android" OFF)
endif()


#  Dependencies 

# nlohmann_json is declared and made available in the top-level CMakeLists.txt

# Check if ONNX Runtime is needed by any enabled module
set(VASDK_NEEDS_ONNXRUNTIME OFF)
if(VASDK_BUILD_SILERO_VAD OR VASDK_BUILD_PARAKEET OR VASDK_BUILD_MOONSHINE_STT OR VASDK_BUILD_SUPERTONIC_TTS OR VASDK_BUILD_SMART_TURN_V3 OR VASDK_BUILD_POCKET_TTS)
    set(VASDK_NEEDS_ONNXRUNTIME ON)
endif()

# ONNX Runtime (SileroVAD, Parakeet, SupertonicTTS)
if(VASDK_NEEDS_ONNXRUNTIME)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindONNXRuntime.cmake)
endif()

# Model downloads
if(VASDK_BUILD_SILERO_VAD)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadSileroVADModel.cmake)
endif()

# FFmpeg (audio file decoding for FFmpegAudioSource)
if(VASDK_BUILD_FFMPEG_AUDIO AND NOT ANDROID)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(FFmpeg)
    if(NOT FFmpeg_FOUND)
        message(WARNING "FFmpeg not found - disabling FFmpeg audio provider")
        set(VASDK_BUILD_FFMPEG_AUDIO OFF CACHE BOOL "Build FFmpeg audio file provider" FORCE)
    endif()
endif()

# RNNoise noise-suppression provider
if(VASDK_BUILD_RNNOISE AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchRNNoise.cmake)
endif()

# VizAnim
if(VASDK_BUILD_VIZANIM)
    include(${CMAKE_CURRENT_SOURCE_DIR}/VizAnim/CMakeLists.txt)
endif()

if(VASDK_BUILD_PARAKEET AND NOT ANDROID)
    set(VASDK_DOWNLOAD_PARAKEET_MODEL ON CACHE BOOL "Download Parakeet ONNX model files")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadParakeetModel.cmake)
endif()

if(VASDK_BUILD_SUPERTONIC_TTS AND NOT ANDROID)
    set(VASDK_DOWNLOAD_SUPERTONIC_MODELS ON CACHE BOOL "Download Supertonic 2 TTS model files")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadSupertonicModels.cmake)
endif()

if(VASDK_BUILD_POCKET_TTS AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadPocketTTSModels.cmake)
endif()

if(VASDK_BUILD_MOONSHINE_STT)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadMoonshineModel.cmake)
endif()

if(VASDK_BUILD_WHISPERCPP AND NOT ANDROID)
    set(VASDK_DOWNLOAD_WHISPERCPP_MODEL ON CACHE BOOL "Download whisper.cpp GGML model file")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadWhisperCppModel.cmake)
endif()

if(VASDK_BUILD_SMART_TURN_V3 AND NOT ANDROID)
    set(VASDK_DOWNLOAD_SMARTTURN_V3_MODEL ON CACHE BOOL "Download SmartTurn V3 ONNX model file")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadSmartTurnModel.cmake)
endif()

if(VASDK_BUILD_LLAMACPP AND NOT ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadLlamaCppModel.cmake)
endif()

# miniaudio (single-header cross-platform audio I/O)
# Download only the single header - no git clone needed.
if(VASDK_BUILD_MINIAUDIO)
    set(_miniaudio_header "${CMAKE_BINARY_DIR}/_deps/miniaudio-src/miniaudio.h")
    if(NOT EXISTS "${_miniaudio_header}")
        message(STATUS "Downloading miniaudio.h 0.11.21")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/miniaudio-src")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/mackron/miniaudio/0.11.21/miniaudio.h"
            "${_miniaudio_header}"
            SHOW_PROGRESS
        )
    endif()
    set(miniaudio_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/miniaudio-src" CACHE PATH "" FORCE)
endif()

# SDL3 (audio I/O)
if(VASDK_BUILD_SDL)
    fetchcontent_declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG "release-3.2.16"
    )
    fetchcontent_makeavailable(SDL3)
endif()

# gladiapp (Gladia STT WebSocket client)
if(VASDK_BUILD_GLADIAPP)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    fetchcontent_declare(
        gladiapp
        GIT_REPOSITORY https://github.com/fatehmtd/gladiapp.git
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(gladiapp)
endif()

# deepgrampp (Deepgram STT/TTS WebSocket client)
if(VASDK_BUILD_DEEPGRAMPP_STT OR VASDK_BUILD_DEEPGRAMPP_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    set(DEEPGRAMPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        deepgrampp
        GIT_REPOSITORY https://github.com/fatehmtd/deepgrampp.git
        GIT_TAG        main
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(deepgrampp)
endif()

# sonioxpp (Soniox STT/TTS WebSocket clients)
if(VASDK_BUILD_SONIOXPP_STT OR VASDK_BUILD_SONIOXPP_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    set(SONIOX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        sonioxpp
        GIT_REPOSITORY https://github.com/fatehmtd/SonioxPP.git
        GIT_TAG        main
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(sonioxpp)

    fetchcontent_declare(
        cppcodec
        GIT_REPOSITORY https://github.com/tplgy/cppcodec.git
        GIT_TAG        master
        GIT_SHALLOW    ON
    )
    fetchcontent_getproperties(cppcodec)
    if(NOT cppcodec_POPULATED)
        fetchcontent_populate(cppcodec)
    endif()
endif()

# cartesiapp (Cartesia STT/TTS REST + WebSocket client)
if(VASDK_BUILD_CARTESIAPP_STT OR VASDK_BUILD_CARTESIAPP_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    set(CARTESIAPP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        cartesiapp
        GIT_REPOSITORY https://github.com/fatehmtd/CartesiaPP.git
        GIT_TAG        main
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(cartesiapp)
endif()

# fishaudiopp (Fish Audio STT/TTS REST + WebSocket client)
if(VASDK_BUILD_FISHAUDIOPP_STT OR VASDK_BUILD_FISHAUDIOPP_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    set(FISHAUDIOPP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        fishaudiopp
        GIT_REPOSITORY https://github.com/fatehmtd/fishaudiopp.git
        GIT_TAG        master
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(fishaudiopp)
endif()

# gradiumpp (Gradium STT/TTS REST + WebSocket client)
# NOTE: placed after cartesiapp/sonioxpp/fishaudiopp on purpose. GradiumPP's own
# CMakeLists.txt unconditionally fetches curl/libwebsockets/spdlog/nlohmann_json
# under the same FetchContent names those libraries use; when one of them runs
# first, GradiumPP's fetch calls become no-ops that reuse the already-populated
# (mbedTLS-backed) targets instead of building a second, OpenSSL-backed copy.
if(VASDK_BUILD_GRADIUMPP_STT OR VASDK_BUILD_GRADIUMPP_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    set(GRADIUM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        gradiumpp
        GIT_REPOSITORY https://github.com/fatehmtd/GradiumPP.git
        GIT_TAG        master
        GIT_SHALLOW    ON
        PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-gradiumpp-cppcodec-guard.cmake
    )
    fetchcontent_makeavailable(gradiumpp)
endif()

# Moonshine bin-tokenizer sources (source-only, no CMake target needed - compiled into MoonshineSTT)
if(VASDK_BUILD_MOONSHINE_STT)
    fetchcontent_declare(
        moonshine_bintokenizer
        GIT_REPOSITORY https://github.com/moonshine-ai/moonshine.git
        GIT_TAG        main
        GIT_SHALLOW    ON
    )
    fetchcontent_getproperties(moonshine_bintokenizer)
    if(NOT moonshine_bintokenizer_POPULATED)
        fetchcontent_populate(moonshine_bintokenizer)
    endif()
endif()

# supertonic TTS helper sources (source-only, no CMake)
if(VASDK_BUILD_SUPERTONIC_TTS)
    # TODO: Pin GIT_TAG to a specific commit SHA before release for reproducibility
    fetchcontent_declare(
        supertonic
        GIT_REPOSITORY https://github.com/supertone-inc/supertonic.git
        GIT_TAG        main
        GIT_SHALLOW    ON
    )
    fetchcontent_getproperties(supertonic)
    if(NOT supertonic_POPULATED)
        fetchcontent_populate(supertonic)
    endif()
endif()

# llama.cpp (local LLM inference via GGUF models)
# Pin to a specific release for reproducibility.
# Force static so llama/ggml are bundled into vasdk_llamacpp.
if(VASDK_BUILD_LLAMACPP)
    set(LLAMA_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER    OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        llamacpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG        b9384
    )

    fetchcontent_getproperties(llamacpp)
    if(NOT llamacpp_POPULATED)
        fetchcontent_populate(llamacpp)
    endif()

    if(llamacpp_POPULATED)
        # On Android: enable OpenMP and ARM CPU optimisations for llama.cpp.
        if(ANDROID)
            set(GGML_OPENMP   ON  CACHE BOOL "" FORCE)
            set(GGML_NATIVE   OFF CACHE BOOL "" FORCE)  # toolchain already sets -march=armv8-a
            # Force-enable NEON/dotprod/fp16 that NDK clang supports but ggml doesn't auto-detect
            set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod+fp16" CACHE STRING "" FORCE)
        endif()
    endif()

    # Force static builds so llama/ggml/pffft are linked into vasdk_llamacpp
    # rather than emitting separate DLLs that must be distributed alongside
    # the executable.  Set this immediately before add_subdirectory so it
    # only affects llama.cpp and its sub-dependencies (pffft etc.), not any
    # previously configured targets such as SDL3.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${llamacpp_SOURCE_DIR} ${llamacpp_BINARY_DIR})
endif()

# whisper.cpp (local Whisper STT)
# Pin to a specific release for reproducible builds; update as needed.
# Force static so whisper/ggml are linked into vasdk_whispercpp rather than
# emitting separate DLLs that would need to be distributed alongside the exe.
# BUILD_SHARED_LIBS may be ON at this point (e.g. set by SDL3), so we
# override it explicitly for whisper.cpp's configuration.
if(VASDK_BUILD_WHISPERCPP)
    set(WHISPER_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER  OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
    fetchcontent_declare(
        whispercpp
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG        v1.7.4  # pinned for reproducible builds; update to upgrade
    )
    fetchcontent_makeavailable(whispercpp)
endif()

# Propagate key variables to parent scope for DLL copy steps in consumer targets
if(VASDK_NEEDS_ONNXRUNTIME)
    set(onnxruntime_SOURCE_DIR "${onnxruntime_SOURCE_DIR}" PARENT_SCOPE)
endif()

if(VASDK_BUILD_LLAMACPP AND DEFINED VASDK_LLAMACPP_MODEL_FILENAME)
    set(VASDK_LLAMACPP_MODEL_FILENAME "${VASDK_LLAMACPP_MODEL_FILENAME}" PARENT_SCOPE)
endif()

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    foreach(_tgt
        vasdk_vizanim
        vasdk_silero_vad
        vasdk_parakeet
        vasdk_pocket_tts
        vasdk_smartturn_v3
    )
        if(TARGET ${_tgt})
            target_link_libraries(${_tgt} PRIVATE vasdk_ctrack_iface)
        endif()
    endforeach()
endif()

#  Provider libraries 

# Determine library type based on user option
if(VASDK_THIRDPARTY_SHARED_LIBS)
    set(VASDK_THIRDPARTY_LIB_TYPE SHARED)
else()
    set(VASDK_THIRDPARTY_LIB_TYPE STATIC)
endif()

if(VASDK_BUILD_FFMPEG_AUDIO)
    add_subdirectory(FFmpegAudio)
endif()

if(VASDK_BUILD_MINIAUDIO)
    add_subdirectory(MiniAudio)
endif()

if(VASDK_BUILD_SDL)
    add_subdirectory(SDL)
endif()

if(VASDK_BUILD_SILERO_VAD)
    add_subdirectory(SileroVAD)
endif()

if(VASDK_BUILD_ENERGY_ZCR_VAD)
    add_subdirectory(EnergyZCRVAD)
endif()

if(VASDK_BUILD_GLADIAPP)
    add_subdirectory(GladiappTranscription)
endif()

if(VASDK_BUILD_SONIOXPP_STT)
    add_subdirectory(SonioxppTranscription)
endif()

if(VASDK_BUILD_DEEPGRAMPP_STT)
    add_subdirectory(DeepgramppTranscription)
endif()

if(VASDK_BUILD_CARTESIAPP_STT)
    add_subdirectory(CartesiappTranscription)
endif()

if(VASDK_BUILD_FISHAUDIOPP_STT)
    add_subdirectory(FishaudioppTranscription)
endif()

if(VASDK_BUILD_GRADIUMPP_STT)
    add_subdirectory(GradiumppTranscription)
endif()

if(VASDK_BUILD_PARAKEET)
    add_subdirectory(Parakeet)
endif()

if(VASDK_BUILD_WHISPERCPP)
    add_subdirectory(WhisperCpp)
endif()

if(VASDK_BUILD_MOONSHINE_STT)
    add_subdirectory(MoonshineSTT)
endif()

if(VASDK_BUILD_DEEPGRAMPP_TTS)
    add_subdirectory(DeepgramppVoiceSynthesis)
endif()

if(VASDK_BUILD_CARTESIAPP_TTS)
    add_subdirectory(CartesiappVoiceSynthesis)
endif()

if(VASDK_BUILD_FISHAUDIOPP_TTS)
    add_subdirectory(FishaudioppVoiceSynthesis)
endif()

if(VASDK_BUILD_GRADIUMPP_TTS)
    add_subdirectory(GradiumppVoiceSynthesis)
endif()

if(VASDK_BUILD_SONIOXPP_TTS)
    add_subdirectory(SonioxppVoiceSynthesis)
endif()

if(VASDK_BUILD_SUPERTONIC_TTS)
    add_subdirectory(SupertonicTTS)
endif()

if(VASDK_BUILD_POCKET_TTS)
    add_subdirectory(PocketTTS)
endif()

if(VASDK_BUILD_RNNOISE)
    add_subdirectory(RNNoise)
endif()

if(VASDK_BUILD_SMART_TURN_V3)
    add_subdirectory(SmartTurnV3)
endif()

if(VASDK_BUILD_LLAMACPP)
    add_subdirectory(LlamaCpp)
endif()
