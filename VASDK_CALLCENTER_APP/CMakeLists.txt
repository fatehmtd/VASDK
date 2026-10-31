cmake_minimum_required(VERSION 3.16...4.0)

project(VASDK_CALLCENTER_APP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

#  Requirement checks (mirror VASDK_APP logic) 

set(CC_REQUIRED_ENABLED TRUE)
set(CC_MISSING "")

if(NOT VASDK_BUILD_SDL AND NOT VASDK_BUILD_MINIAUDIO)
    set(CC_REQUIRED_ENABLED FALSE)
    list(APPEND CC_MISSING "SDL or MINIAUDIO (audio I/O)")
endif()

if(NOT VASDK_BUILD_SILERO_VAD)
    set(CC_REQUIRED_ENABLED FALSE)
    list(APPEND CC_MISSING "SILERO_VAD")
endif()

if(NOT (VASDK_BUILD_PARAKEET OR VASDK_BUILD_MOONSHINE_STT OR VASDK_BUILD_WHISPERCPP OR VASDK_BUILD_WHISTLE_STT OR VASDK_BUILD_GLADIAPP OR VASDK_BUILD_SONIOXPP_STT OR VASDK_BUILD_DEEPGRAMPP_STT OR VASDK_BUILD_CARTESIAPP_STT OR VASDK_BUILD_FISHAUDIOPP_STT OR VASDK_BUILD_GRADIUMPP_STT))
    set(CC_REQUIRED_ENABLED FALSE)
    list(APPEND CC_MISSING "At least one STT provider (PARAKEET, MOONSHINE_STT, WHISPERCPP, WHISTLE_STT, GLADIAPP, SONIOXPP_STT, DEEPGRAMPP_STT, CARTESIAPP_STT, FISHAUDIOPP_STT, GRADIUMPP_STT)")
endif()

if(NOT (VASDK_BUILD_POCKET_TTS OR VASDK_BUILD_PHONON_TTS OR VASDK_BUILD_MOSS_TTS OR VASDK_BUILD_SUPERTONIC_TTS OR VASDK_BUILD_DEEPGRAMPP_TTS OR VASDK_BUILD_CARTESIAPP_TTS OR VASDK_BUILD_FISHAUDIOPP_TTS OR VASDK_BUILD_GRADIUMPP_TTS OR VASDK_BUILD_SONIOXPP_TTS))
    set(CC_REQUIRED_ENABLED FALSE)
    list(APPEND CC_MISSING "At least one TTS provider (POCKET_TTS, PHONON_TTS, MOSS_TTS, SUPERTONIC_TTS, DEEPGRAMPP_TTS, CARTESIAPP_TTS, FISHAUDIOPP_TTS, GRADIUMPP_TTS, SONIOXPP_TTS)")
endif()

if(NOT CC_REQUIRED_ENABLED)
    message(WARNING
        "VASDK_CallCenter requires the following modules but they are disabled:\n"
        "  ${CC_MISSING}\n"
        "Skipping VASDK_CallCenter build. Enable required modules with -DVASDK_BUILD_<MODULE>=ON")
    return()
endif()

#  Executable 

add_executable(VASDK_CallCenter callcenter_pipeline.cpp)

target_link_libraries(VASDK_CallCenter
    PRIVATE VASDK
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE CLI11::CLI11
    PRIVATE spdlog::spdlog
    PRIVATE dotenv
)

#  Audio I/O 

if(VASDK_BUILD_MINIAUDIO AND TARGET vasdk_miniaudio_audio)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_miniaudio_audio)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_MINIAUDIO)
elseif(VASDK_BUILD_SDL AND TARGET vasdk_sdl_audio)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_sdl_audio)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SDL)
endif()

#  VAD 

if(VASDK_BUILD_SILERO_VAD AND TARGET vasdk_silero_vad)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_silero_vad)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SILERO_VAD)
endif()

#  STT 

if(VASDK_BUILD_MOONSHINE_STT AND TARGET vasdk_moonshine_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_moonshine_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_MOONSHINE_STT)
    message(STATUS "[VASDK_CallCenter] Moonshine STT enabled")
endif()

if(VASDK_BUILD_PARAKEET AND TARGET vasdk_parakeet)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_parakeet)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_PARAKEET)
    message(STATUS "[VASDK_CallCenter] Parakeet STT enabled")
endif()

if(VASDK_BUILD_WHISPERCPP AND TARGET vasdk_whispercpp)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_whispercpp)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_WHISPERCPP)
    message(STATUS "[VASDK_CallCenter] Whisper.cpp STT enabled")
endif()

if(VASDK_BUILD_WHISTLE_STT AND TARGET vasdk_whistle_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_whistle_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_WHISTLE_STT)
    message(STATUS "[VASDK_CallCenter] Whistle STT enabled")
endif()

if(VASDK_BUILD_GLADIAPP AND TARGET vasdk_gladiapp_transcription)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_gladiapp_transcription)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_GLADIAPP)
    message(STATUS "[VASDK_CallCenter] Gladia STT enabled")
endif()

if(VASDK_BUILD_SONIOXPP_STT AND TARGET vasdk_sonioxpp_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_sonioxpp_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SONIOXPP_STT)
    message(STATUS "[VASDK_CallCenter] Soniox STT enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_STT AND TARGET vasdk_deepgrampp_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_deepgrampp_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_DEEPGRAMPP_STT)
    message(STATUS "[VASDK_CallCenter] Deepgram STT enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_STT AND TARGET vasdk_cartesiapp_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_cartesiapp_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_CARTESIAPP_STT)
    message(STATUS "[VASDK_CallCenter] Cartesia STT enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_STT AND TARGET vasdk_fishaudiopp_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_fishaudiopp_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_FISHAUDIOPP_STT)
    message(STATUS "[VASDK_CallCenter] Fish Audio STT enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_STT AND TARGET vasdk_gradiumpp_stt)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_gradiumpp_stt)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_GRADIUMPP_STT)
    message(STATUS "[VASDK_CallCenter] Gradium STT enabled")
endif()

#  TTS

if(VASDK_BUILD_POCKET_TTS AND TARGET vasdk_pocket_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_pocket_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_POCKET_TTS)
    message(STATUS "[VASDK_CallCenter] PocketTTS enabled")
endif()

if(VASDK_BUILD_PHONON_TTS AND TARGET vasdk_phonon_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_phonon_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_PHONON_TTS)
    message(STATUS "[VASDK_CallCenter] Phonon TTS enabled")
endif()

if(VASDK_BUILD_MOSS_TTS AND TARGET vasdk_moss_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_moss_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_MOSS_TTS)
    message(STATUS "[VASDK_CallCenter] MOSS-TTS-Nano enabled")
endif()

if(VASDK_BUILD_SUPERTONIC_TTS AND TARGET vasdk_supertonic_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_supertonic_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SUPERTONIC_TTS)
    message(STATUS "[VASDK_CallCenter] Supertonic TTS enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_TTS AND TARGET vasdk_deepgrampp_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_deepgrampp_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_DEEPGRAMPP_TTS)
    message(STATUS "[VASDK_CallCenter] Deepgram TTS enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_TTS AND TARGET vasdk_cartesiapp_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_cartesiapp_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_CARTESIAPP_TTS)
    message(STATUS "[VASDK_CallCenter] Cartesia TTS enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_TTS AND TARGET vasdk_fishaudiopp_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_fishaudiopp_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_FISHAUDIOPP_TTS)
    message(STATUS "[VASDK_CallCenter] Fish Audio TTS enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_TTS AND TARGET vasdk_gradiumpp_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_gradiumpp_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_GRADIUMPP_TTS)
    message(STATUS "[VASDK_CallCenter] Gradium TTS enabled")
endif()

if(VASDK_BUILD_SONIOXPP_TTS AND TARGET vasdk_sonioxpp_tts)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_sonioxpp_tts)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SONIOXPP_TTS)
    message(STATUS "[VASDK_CallCenter] Soniox TTS enabled")
endif()

#  Optional: Smart turn detection 

if(VASDK_BUILD_SMART_TURN_V3 AND TARGET vasdk_smartturn_v3)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_smartturn_v3)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_SMART_TURN_V3)
    message(STATUS "[VASDK_CallCenter] SmartTurn v3 enabled")
endif()

#  Optional: Speaker tracking / identification

if(VASDK_BUILD_PYANNOTE_SPEAKER AND TARGET vasdk_pyannote_speaker)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_pyannote_speaker)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_PYANNOTE_SPEAKER)
    message(STATUS "[VASDK_CallCenter] pyannote speaker tracking enabled")
endif()

#  Optional: RNNoise noise suppression 

if(VASDK_BUILD_RNNOISE AND TARGET vasdk_rnnoise)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_rnnoise)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_RNNOISE)
    message(STATUS "[VASDK_CallCenter] RNNoise noise suppression enabled")
endif()

#  Optional: llama.cpp local LLM 

if(VASDK_BUILD_LLAMACPP AND TARGET vasdk_llamacpp)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_llamacpp)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_LLAMACPP)
    message(STATUS "[VASDK_CallCenter] llama.cpp local LLM enabled")
endif()

#  ctrack profiling 

if(VASDK_ENABLE_CTRACK AND TARGET vasdk_ctrack_iface)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_ctrack_iface)
endif()

#  Windows runtime DLLs 

if(WIN32)
    add_custom_command(TARGET VASDK_CallCenter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
            $<TARGET_FILE_DIR:VASDK_CallCenter>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${onnxruntime_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll"
            $<TARGET_FILE_DIR:VASDK_CallCenter>
    )
    if(VASDK_BUILD_SDL AND TARGET SDL3::SDL3)
        add_custom_command(TARGET VASDK_CallCenter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3>
                $<TARGET_FILE_DIR:VASDK_CallCenter>
        )
    endif()
    add_custom_command(TARGET VASDK_CallCenter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:VASDK>
            $<TARGET_FILE_DIR:VASDK_CallCenter>
    )
    # An installed (e.g. vcpkg) libcurl is a DLL that VASDK.dll imports, along with
    # zlib; a libcurl built from source (VASDK_CURL_BUNDLED) is linked statically.
    if(VASDK_ENABLE_HTTP_TRANSPORT AND NOT VASDK_CURL_BUNDLED)
        add_custom_command(TARGET VASDK_CallCenter POST_BUILD
            # debug: libcurl-d.dll, release: libcurl.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,libcurl-d.dll,libcurl.dll>"
                $<TARGET_FILE_DIR:VASDK_CallCenter>
            # debug: zlibd1.dll, release: zlib1.dll
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:VASDK>/$<IF:$<CONFIG:Debug>,zlibd1.dll,zlib1.dll>"
                $<TARGET_FILE_DIR:VASDK_CallCenter>
        )
    endif()
endif()

#  Install 

install(TARGETS VASDK_CallCenter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
