cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK_APP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# Check if required modules are enabled for VASDK_Pipeline
set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED TRUE)
set(VASDK_PIPELINE_MISSING_MODULES "")

if(NOT VASDK_BUILD_SDL AND NOT VASDK_BUILD_MINIAUDIO)
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "SDL or MINIAUDIO")
endif()

if(NOT VASDK_BUILD_SILERO_VAD)
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "SILERO_VAD")
endif()

# Require at least one transcription provider
if(NOT (VASDK_BUILD_GLADIAPP OR VASDK_BUILD_SONIOXPP_STT OR VASDK_BUILD_DEEPGRAMPP_STT OR VASDK_BUILD_CARTESIAPP_STT OR VASDK_BUILD_FISHAUDIOPP_STT OR VASDK_BUILD_GRADIUMPP_STT OR VASDK_BUILD_PARAKEET OR VASDK_BUILD_WHISPERCPP OR VASDK_BUILD_MOONSHINE_STT OR VASDK_BUILD_WHISTLE_STT))
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "At least one transcription provider (GLADIAPP, SONIOXPP_STT, DEEPGRAMPP_STT, CARTESIAPP_STT, FISHAUDIOPP_STT, GRADIUMPP_STT, PARAKEET, WHISPERCPP, MOONSHINE_STT, or WHISTLE_STT)")
endif()

# Require at least one TTS provider
if(NOT (VASDK_BUILD_DEEPGRAMPP_TTS OR VASDK_BUILD_CARTESIAPP_TTS OR VASDK_BUILD_FISHAUDIOPP_TTS OR VASDK_BUILD_GRADIUMPP_TTS OR VASDK_BUILD_SONIOXPP_TTS OR VASDK_BUILD_SUPERTONIC_TTS OR VASDK_BUILD_POCKET_TTS OR VASDK_BUILD_PHONON_TTS))
    set(VASDK_PIPELINE_REQUIRED_MODULES_ENABLED FALSE)
    list(APPEND VASDK_PIPELINE_MISSING_MODULES "At least one TTS provider (DEEPGRAMPP_TTS, CARTESIAPP_TTS, FISHAUDIOPP_TTS, GRADIUMPP_TTS, SONIOXPP_TTS, SUPERTONIC_TTS, POCKET_TTS, or PHONON_TTS)")
endif()

if(NOT VASDK_PIPELINE_REQUIRED_MODULES_ENABLED)
    message(WARNING "VASDK_Pipeline requires the following modules but they are disabled: ${VASDK_PIPELINE_MISSING_MODULES}")
    message(WARNING "Skipping VASDK_Pipeline build. Enable required modules with -DVASDK_BUILD_<MODULE>=ON")
    return()
endif()

# Collect TUI sources when FTXUI is available
set(VASDK_PIPELINE_TUI_SOURCES "")
if(VASDK_BUILD_TUI AND TARGET ftxui::component)
    list(APPEND VASDK_PIPELINE_TUI_SOURCES TUI/PipelineTUI.cpp)
endif()

# Add pipeline-mode executable with LLM support
add_executable(VASDK_Pipeline main_pipeline.cpp ${VASDK_PIPELINE_TUI_SOURCES})

# Link libraries conditionally based on enabled modules
target_link_libraries(VASDK_Pipeline
    PRIVATE VASDK
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE CLI11::CLI11
    PRIVATE dotenv
)

# Conditionally link 3rd party modules based on build options
if(VASDK_BUILD_MINIAUDIO AND TARGET vasdk_miniaudio_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_miniaudio_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_MINIAUDIO)
elseif(VASDK_BUILD_SDL AND TARGET vasdk_sdl_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_sdl_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SDL)
endif()

if(VASDK_BUILD_SILERO_VAD AND TARGET vasdk_silero_vad)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_silero_vad)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SILERO_VAD)
endif()

if(VASDK_BUILD_ENERGY_ZCR_VAD AND TARGET vasdk_energy_zcr_vad)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_energy_zcr_vad)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_ENERGY_ZCR_VAD)
endif()

if(VASDK_BUILD_GLADIAPP AND TARGET vasdk_gladiapp_transcription)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_gladiapp_transcription)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_GLADIAPP)
endif()

if(VASDK_BUILD_SONIOXPP_STT AND TARGET vasdk_sonioxpp_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_sonioxpp_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SONIOXPP_STT)
    message(STATUS "[VASDK_Pipeline] Soniox STT provider enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_STT AND TARGET vasdk_deepgrampp_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_deepgrampp_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_DEEPGRAMPP_STT)
    message(STATUS "[VASDK_Pipeline] Deepgram STT provider enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_STT AND TARGET vasdk_cartesiapp_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_cartesiapp_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_CARTESIAPP_STT)
    message(STATUS "[VASDK_Pipeline] Cartesia STT provider enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_STT AND TARGET vasdk_fishaudiopp_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_fishaudiopp_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FISHAUDIOPP_STT)
    message(STATUS "[VASDK_Pipeline] Fish Audio STT provider enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_STT AND TARGET vasdk_gradiumpp_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_gradiumpp_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_GRADIUMPP_STT)
    message(STATUS "[VASDK_Pipeline] Gradium STT provider enabled")
endif()

if(VASDK_BUILD_PARAKEET AND TARGET vasdk_parakeet)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_parakeet)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_PARAKEET)
endif()

if(VASDK_BUILD_WHISPERCPP AND TARGET vasdk_whispercpp)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_whispercpp)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_WHISPERCPP)
endif()

if(VASDK_BUILD_MOONSHINE_STT AND TARGET vasdk_moonshine_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_moonshine_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_MOONSHINE_STT)
    message(STATUS "[VASDK_Pipeline] Moonshine STT provider enabled")
endif()

if(VASDK_BUILD_WHISTLE_STT AND TARGET vasdk_whistle_stt)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_whistle_stt)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_WHISTLE_STT)
    message(STATUS "[VASDK_Pipeline] Whistle STT provider enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_TTS AND TARGET vasdk_deepgrampp_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_deepgrampp_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_DEEPGRAMPP_TTS)
    message(STATUS "[VASDK_Pipeline] Deepgram TTS provider enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_TTS AND TARGET vasdk_cartesiapp_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_cartesiapp_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_CARTESIAPP_TTS)
    message(STATUS "[VASDK_Pipeline] Cartesia TTS provider enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_TTS AND TARGET vasdk_fishaudiopp_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_fishaudiopp_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FISHAUDIOPP_TTS)
    message(STATUS "[VASDK_Pipeline] Fish Audio TTS provider enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_TTS AND TARGET vasdk_gradiumpp_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_gradiumpp_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_GRADIUMPP_TTS)
    message(STATUS "[VASDK_Pipeline] Gradium TTS provider enabled")
endif()

if(VASDK_BUILD_SONIOXPP_TTS AND TARGET vasdk_sonioxpp_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_sonioxpp_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SONIOXPP_TTS)
    message(STATUS "[VASDK_Pipeline] Soniox TTS provider enabled")
endif()

if(VASDK_BUILD_SUPERTONIC_TTS AND TARGET vasdk_supertonic_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_supertonic_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SUPERTONIC_TTS)
endif()

if(VASDK_BUILD_POCKET_TTS AND TARGET vasdk_pocket_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_pocket_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_POCKET_TTS)
    message(STATUS "[VASDK_Pipeline] PocketTTS provider enabled")
endif()

if(VASDK_BUILD_PHONON_TTS AND TARGET vasdk_phonon_tts)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_phonon_tts)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_PHONON_TTS)
    message(STATUS "[VASDK_Pipeline] Phonon TTS provider enabled")
endif()

if(VASDK_BUILD_FFMPEG_AUDIO AND TARGET vasdk_ffmpeg_audio)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_ffmpeg_audio)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FFMPEG_AUDIO)
endif()

# Link the RNNoise DSP provider if it was built
if(VASDK_BUILD_RNNOISE AND TARGET vasdk_rnnoise)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_rnnoise)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_RNNOISE)
    message(STATUS "[VASDK_Pipeline] RNNoise noise-suppression enabled")
endif()

# Link the VizAnim provider if it was built
if(VASDK_BUILD_VIZANIM AND TARGET vasdk_vizanim)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_vizanim)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_VIZANIM)
    message(STATUS "[VASDK_Pipeline] VizAnim audio feature extraction enabled")
endif()

# Link the SmartTurn v3 provider if it was built
if(VASDK_BUILD_SMART_TURN_V3 AND TARGET vasdk_smartturn_v3)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_smartturn_v3)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_SMART_TURN_V3)
    message(STATUS "[VASDK_Pipeline] SmartTurn v3 turn detection enabled")
endif()

# Link the pyannote speaker embedding provider if it was built
if(VASDK_BUILD_PYANNOTE_SPEAKER AND TARGET vasdk_pyannote_speaker)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_pyannote_speaker)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_PYANNOTE_SPEAKER)
    message(STATUS "[VASDK_Pipeline] pyannote speaker tracking enabled")
endif()

if(VASDK_BUILD_TUI AND TARGET ftxui::component)
    target_link_libraries(VASDK_Pipeline PRIVATE ftxui::component ftxui::dom ftxui::screen)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_FTXUI)
    target_include_directories(VASDK_Pipeline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    message(STATUS "[VASDK_Pipeline] FTXUI terminal UI enabled")
endif()

if(VASDK_BUILD_LLAMACPP AND TARGET vasdk_llamacpp)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_llamacpp)
    target_compile_definitions(VASDK_Pipeline PRIVATE VASDK_HAS_LLAMACPP)
    if(DEFINED VASDK_LLAMACPP_MODEL_FILENAME AND NOT VASDK_LLAMACPP_MODEL_FILENAME STREQUAL "")
        target_compile_definitions(VASDK_Pipeline PRIVATE
            VASDK_LLAMACPP_DEFAULT_MODEL="${VASDK_LLAMACPP_MODEL_FILENAME}")
        message(STATUS "[VASDK_Pipeline] llama.cpp default model: ${VASDK_LLAMACPP_MODEL_FILENAME}")
    endif()
    message(STATUS "[VASDK_Pipeline] llama.cpp local LLM provider enabled")
endif()

# Copy runtime DLLs to output directory for VASDK_Pipeline
if(WIN32)
    # Copy ONNX Runtime DLLs
    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
        $<TARGET_FILE_DIR:VASDK_Pipeline>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${onnxruntime_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll"
        $<TARGET_FILE_DIR:VASDK_Pipeline>
    )
    # Copy SDL3 DLL (only when SDL is enabled)
    if(VASDK_BUILD_SDL AND TARGET SDL3::SDL3)
        add_custom_command(TARGET VASDK_Pipeline POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3>
            $<TARGET_FILE_DIR:VASDK_Pipeline>
        )
    endif()
    # Copy llama.cpp runtime DLLs (llama, ggml, ggml-base, ggml-cpu, pffft).
    # After the BUILD_SHARED_LIBS=OFF fix these will be static and this loop
    # becomes a no-op; it is kept as a safety net for pre-existing shared builds.
    if(VASDK_BUILD_LLAMACPP)
        foreach(_llama_tgt llama ggml ggml-base ggml-cpu PFFFT)
            if(TARGET ${_llama_tgt})
                get_target_property(_llama_tgt_type ${_llama_tgt} TYPE)
                if(_llama_tgt_type STREQUAL "SHARED_LIBRARY")
                    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "$<TARGET_FILE:${_llama_tgt}>"
                            "$<TARGET_FILE_DIR:VASDK_Pipeline>"
                    )
                endif()
            endif()
        endforeach()
    endif()
    # Copy VASDK DLL and its runtime dependencies
    add_custom_command(TARGET VASDK_Pipeline POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:VASDK>
        $<TARGET_FILE_DIR:VASDK_Pipeline>
    )
    # An installed (e.g. vcpkg) libcurl is a DLL that VASDK.dll imports, along with
    # zlib; a libcurl built from source (VASDK_CURL_BUNDLED) is linked statically.
    if(VASDK_ENABLE_HTTP_TRANSPORT AND NOT VASDK_CURL_BUNDLED)
        add_custom_command(TARGET VASDK_Pipeline POST_BUILD
            # debug: libcurl-d.dll, release: libcurl.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,libcurl-d.dll,libcurl.dll>"
                $<TARGET_FILE_DIR:VASDK_Pipeline>
            # debug: zlibd1.dll, release: zlib1.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,zlibd1.dll,zlib1.dll>"
                $<TARGET_FILE_DIR:VASDK_Pipeline>
        )
    endif()
endif()

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    target_link_libraries(VASDK_Pipeline PRIVATE vasdk_ctrack_iface)
endif()

install(TARGETS VASDK_Pipeline
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install model files to the installation directory
install(DIRECTORY "${CMAKE_BINARY_DIR}/models/"
    DESTINATION "bin/models/"
    OPTIONAL)

# Post-build step to copy all shared libraries to the install directory
install(CODE "
    # Find and install all shared libraries in build directories
    file(GLOB_RECURSE SHARED_LIBS
        \"${CMAKE_BINARY_DIR}/*.dll\"
        \"${CMAKE_BINARY_DIR}/*.so\"
        \"${CMAKE_BINARY_DIR}/*.dylib\"
    )
    foreach(lib \${SHARED_LIBS})
        get_filename_component(lib_name \${lib} NAME)
        # Skip system libraries and debug symbols
        if(NOT lib_name MATCHES \"(msvc|ucrt|api-ms|ext-ms|.*d\\.dll)\" AND
           NOT lib MATCHES \"CMakeFiles\")
            file(INSTALL \${lib} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
        endif()
    endforeach()
")
