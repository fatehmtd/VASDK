# Provider catalog: registers every VAD/STT/TTS provider built in this tree with a
# va::engine::ProviderRegistry, so apps pick providers by id from an engine config.
add_library(vasdk_provider_catalog STATIC
    ProviderCatalog.cpp
    proto/vasdk/providers/options.proto
)
protobuf_generate(TARGET vasdk_provider_catalog
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)

target_include_directories(vasdk_provider_catalog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/proto>
)
target_link_libraries(vasdk_provider_catalog PUBLIC VASDK)

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
    endif()
endforeach()

# A spoken conversation with no network, through the C API: VAD, STT and TTS on this machine.
if(VASDK_BUILD_TESTS AND TARGET vasdk_silero_vad AND TARGET vasdk_whistle_stt AND TARGET vasdk_pocket_tts AND TARGET vasdk_phonon_tts)
    add_executable(vasdk_offline_conversation_test offline_conversation_test.cpp)
    target_link_libraries(vasdk_offline_conversation_test PRIVATE vasdk_provider_catalog nlohmann_json::nlohmann_json)
    add_test(NAME vasdk_offline_conversation
             COMMAND vasdk_offline_conversation_test ${CMAKE_CURRENT_BINARY_DIR}/../models)
    set_tests_properties(vasdk_offline_conversation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    vasdk_test_dlls(vasdk_offline_conversation_test)
endif()
