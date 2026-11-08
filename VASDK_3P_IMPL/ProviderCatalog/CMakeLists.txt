# Provider catalog: registers every VAD/STT/TTS provider built in this tree with a
# va::engine::ProviderRegistry, so apps pick providers by id from an engine config.
add_library(vasdk_provider_catalog STATIC
    ProviderCatalog.cpp
    ProviderCatalogC.cpp
    proto/vasdk/providers/options.proto
)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto)   # protoc doesn't create its output folder
protobuf_generate(TARGET vasdk_provider_catalog
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)

target_include_directories(vasdk_provider_catalog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/proto>
)
target_link_libraries(vasdk_provider_catalog PUBLIC VASDK)
add_library(VASDK::providers ALIAS vasdk_provider_catalog)   # every provider built, for a project that adds VASDK from source

# target  ->  define the catalog uses to register it
set(_vasdk_catalog_providers
    vasdk_silero_vad      SILERO_VAD
    vasdk_gradiumpp_stt   GRADIUMPP_STT
    vasdk_sonioxpp_stt    SONIOXPP_STT
    vasdk_deepgrampp_stt  DEEPGRAMPP_STT
    vasdk_cartesiapp_stt  CARTESIAPP_STT
    vasdk_fishaudiopp_stt FISHAUDIOPP_STT
    vasdk_gladiapp_transcription GLADIAPP
    vasdk_whistle_stt     WHISTLE_STT
    vasdk_moonshine_stt   MOONSHINE_STT
    vasdk_parakeet        PARAKEET
    vasdk_gradiumpp_tts   GRADIUMPP_TTS
    vasdk_sonioxpp_tts    SONIOXPP_TTS
    vasdk_supertonic_tts  SUPERTONIC_TTS
    vasdk_deepgrampp_tts  DEEPGRAMPP_TTS
    vasdk_cartesiapp_tts  CARTESIAPP_TTS
    vasdk_fishaudiopp_tts FISHAUDIOPP_TTS
    vasdk_moss_tts        MOSS_TTS
    vasdk_phonon_tts      PHONON_TTS
    vasdk_pocket_tts      POCKET_TTS
    vasdk_smartturn_v3    SMART_TURN_V3
    vasdk_minilm_nlu      MINILM_NLU
    vasdk_gliner_ner      GLINER_NER
    vasdk_libdatachannel_webrtc LIBDATACHANNEL_WEBRTC
    vasdk_miniaudio_audio MINIAUDIO
    vasdk_aaudio          AAUDIO
    vasdk_llamacpp        LLAMACPP
    vasdk_rnnoise         RNNOISE
    vasdk_pyannote_speaker PYANNOTE_SPEAKER
)
list(LENGTH _vasdk_catalog_providers _count)
math(EXPR _last "${_count} - 1")
foreach(_index RANGE 0 ${_last} 2)
    math(EXPR _define_index "${_index} + 1")
    list(GET _vasdk_catalog_providers ${_index} _target)
    list(GET _vasdk_catalog_providers ${_define_index} _define)
    # Android's Whistle needs its NDK-built engine (none for the x86_64 emulator).
    if(_target STREQUAL "vasdk_whistle_stt" AND ANDROID AND NOT WHISTLE_ENGINE_STATIC)
        continue()
    endif()
    if(TARGET ${_target})
        target_link_libraries(vasdk_provider_catalog PUBLIC ${_target})
        target_compile_definitions(vasdk_provider_catalog PRIVATE VASDK_HAS_${_define})
        # One provider on its own: VASDK::silero_vad, VASDK::whistle_stt, ...
        string(REGEX REPLACE "^vasdk_" "" _alias ${_target})
        if(NOT TARGET VASDK::${_alias})
            add_library(VASDK::${_alias} ALIAS ${_target})
        endif()
    endif()
endforeach()
# The page a config-created WebRTC peer serves when its options name none.
target_compile_definitions(vasdk_provider_catalog PRIVATE
    VASDK_WEBRTC_DEMO_PAGE="${VASDK_ROOT_DIR}/assets/webrtc/index.html")

# The providers as a shared library beside a shared VASDK, for code that loads them at run time:
# Python's vasdk.Providers (bindings/python). Exports vasdk_providers_add_all.
get_target_property(_vasdk_library_type VASDK TYPE)
if(_vasdk_library_type STREQUAL "SHARED_LIBRARY")
    add_library(vasdk_providers SHARED ProvidersShared.cpp)
    target_link_libraries(vasdk_providers PRIVATE vasdk_provider_catalog)
    vasdk_test_dlls(vasdk_providers)   # VASDK.dll, onnxruntime.dll and the rest beside it, on Windows
endif()

# The providers' options as Python messages, vasdk.providers.options_pb2, next to VASDK's own
# (VASDK/CMakeLists.txt, vasdk_python_protos): a config built in code in bindings/python.
if(NOT ANDROID AND (TARGET protobuf::protoc OR Protobuf_PROTOC_EXECUTABLE))
    if(TARGET protobuf::protoc)
        set(_catalog_protoc $<TARGET_FILE:protobuf::protoc>)
    else()
        set(_catalog_protoc ${Protobuf_PROTOC_EXECUTABLE})
    endif()
    set(_catalog_python_out ${VASDK_ROOT_BINARY_DIR}/bindings/python/vasdk/providers)
    file(MAKE_DIRECTORY ${_catalog_python_out})
    if(NOT EXISTS ${_catalog_python_out}/__init__.py)
        file(WRITE ${_catalog_python_out}/__init__.py "")
    endif()
    add_custom_command(OUTPUT ${_catalog_python_out}/options_pb2.py
        COMMAND ${_catalog_protoc} --python_out=${VASDK_ROOT_BINARY_DIR}/bindings/python --pyi_out=${VASDK_ROOT_BINARY_DIR}/bindings/python
                -I ${CMAKE_CURRENT_SOURCE_DIR}/proto vasdk/providers/options.proto
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/proto
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/vasdk/providers/options.proto
        COMMENT "Python messages for the providers' options"
        VERBATIM)
    add_custom_target(vasdk_provider_options_python ALL DEPENDS ${_catalog_python_out}/options_pb2.py)
endif()

# A cloud STT or TTS with no API key gives no provider, so its fallback starts (every one built).
if(VASDK_BUILD_TESTS)
    add_executable(vasdk_cloud_key_test cloud_key_test.cpp)
    target_link_libraries(vasdk_cloud_key_test PRIVATE vasdk_provider_catalog)
    add_test(NAME vasdk_cloud_key COMMAND vasdk_cloud_key_test)
    vasdk_test_dlls(vasdk_cloud_key_test)

    # A local voice's pre-buffer: its own prebuffer_ms, else audio.tts_prebuffer_ms, else 250 ms.
    add_executable(vasdk_tts_prebuffer_test tts_prebuffer_test.cpp)
    target_link_libraries(vasdk_tts_prebuffer_test PRIVATE vasdk_provider_catalog)
    add_test(NAME vasdk_tts_prebuffer COMMAND vasdk_tts_prebuffer_test)
    set_tests_properties(vasdk_tts_prebuffer PROPERTIES SKIP_RETURN_CODE 77)
    vasdk_test_dlls(vasdk_tts_prebuffer_test)

    # Every provider takes its own options message, and the older shared ones still (every one built).
    add_executable(vasdk_provider_options_test provider_options_test.cpp)
    target_link_libraries(vasdk_provider_options_test PRIVATE vasdk_provider_catalog)
    add_test(NAME vasdk_provider_options COMMAND vasdk_provider_options_test)
    set_tests_properties(vasdk_provider_options PROPERTIES SKIP_RETURN_CODE 77)
    vasdk_test_dlls(vasdk_provider_options_test)
endif()

# A spoken conversation with no network, through the C API: VAD, STT and TTS on this machine.
if(VASDK_BUILD_TESTS AND TARGET vasdk_silero_vad AND TARGET vasdk_whistle_stt AND TARGET vasdk_pocket_tts AND TARGET vasdk_phonon_tts)
    add_executable(vasdk_offline_conversation_test offline_conversation_test.cpp)
    target_link_libraries(vasdk_offline_conversation_test PRIVATE vasdk_provider_catalog nlohmann_json::nlohmann_json)
    add_test(NAME vasdk_offline_conversation
             COMMAND vasdk_offline_conversation_test ${CMAKE_CURRENT_BINARY_DIR}/../models)
    set_tests_properties(vasdk_offline_conversation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    vasdk_test_dlls(vasdk_offline_conversation_test)
endif()

# The audio DSP and speaker embedding picked by config id (rnnoise, pyannote), and a session with both.
if(VASDK_BUILD_TESTS AND TARGET vasdk_pyannote_speaker)
    add_executable(vasdk_dsp_speaker_test dsp_speaker_test.cpp)
    target_link_libraries(vasdk_dsp_speaker_test PRIVATE vasdk_provider_catalog)
    add_test(NAME vasdk_dsp_speaker COMMAND vasdk_dsp_speaker_test ${CMAKE_CURRENT_BINARY_DIR}/../models)
    set_tests_properties(vasdk_dsp_speaker PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    vasdk_test_dlls(vasdk_dsp_speaker_test)
endif()

# A spoken conversation over WebRTC, as a browser has it: the config's webrtc peer, called by a
# libdatachannel offerer that speaks Opus. Also calls a running app: <models> --call <port> "<words>".
if(VASDK_BUILD_TESTS AND TARGET vasdk_libdatachannel_webrtc AND TARGET vasdk_silero_vad AND TARGET vasdk_whistle_stt AND TARGET vasdk_phonon_tts)
    add_executable(vasdk_webrtc_session_test webrtc_session_test.cpp)
    target_link_libraries(vasdk_webrtc_session_test PRIVATE vasdk_provider_catalog nlohmann_json::nlohmann_json
        LibDataChannel::LibDataChannelStatic Opus::opus httplib::httplib)
    add_test(NAME vasdk_webrtc_session COMMAND vasdk_webrtc_session_test ${CMAKE_CURRENT_BINARY_DIR}/../models)
    set_tests_properties(vasdk_webrtc_session PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    vasdk_test_dlls(vasdk_webrtc_session_test)
endif()

# An NLU eval set as the agent hears it: spoken by Phonon's voices, recognized by Whistle. For
# vasdk_agentc eval --sweep on recognized speech (the garage set's is in VASDK_APP).
if(TARGET vasdk_whistle_stt AND TARGET vasdk_phonon_tts)
    add_executable(vasdk_spoken_eval spoken_eval.cpp)
    target_link_libraries(vasdk_spoken_eval PRIVATE vasdk_provider_catalog)
    vasdk_test_dlls(vasdk_spoken_eval)
endif()
