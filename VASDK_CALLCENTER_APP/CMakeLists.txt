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

if(NOT VASDK_ENABLE_ENGINE_CONFIG)
    set(CC_REQUIRED_ENABLED FALSE)
    list(APPEND CC_MISSING "VASDK_ENABLE_ENGINE_CONFIG (the call runs as an engine session)")
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

if(VASDK_BUILD_LIBDATACHANNEL_WEBRTC AND TARGET vasdk_libdatachannel_webrtc)
    target_link_libraries(VASDK_CallCenter PRIVATE vasdk_libdatachannel_webrtc)
    target_compile_definitions(VASDK_CallCenter PRIVATE VASDK_HAS_LIBDATACHANNEL_WEBRTC
        VASDK_WEBRTC_DEMO_PAGE="${CMAKE_SOURCE_DIR}/assets/webrtc/index.html")
endif()

# VAD, STT, TTS and turn detection: every provider built in this tree; the call takes the first of each
target_link_libraries(VASDK_CallCenter PRIVATE vasdk_provider_catalog)

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
