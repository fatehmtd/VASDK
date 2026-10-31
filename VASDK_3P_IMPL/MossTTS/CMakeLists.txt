#  SentencePiece
# Shared with PocketTTS / PhononTTS when they are enabled.
if(NOT TARGET sentencepiece-static)
    set(SPM_ENABLE_SHARED   OFF CACHE BOOL "" FORCE)
    set(SPM_BUILD_TEST      OFF CACHE BOOL "" FORCE)
    set(SPM_BUILD_EXAMPLE   OFF CACHE BOOL "" FORCE)
    set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
    # sentencepiece v0.2.0 has cmake_minimum_required(VERSION 2.8); allow it under CMake 4.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(
        sentencepiece
        GIT_REPOSITORY https://github.com/google/sentencepiece.git
        GIT_TAG        v0.2.0
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(sentencepiece)
    if(ANDROID AND TARGET sentencepiece-static)
        target_link_libraries(sentencepiece-static INTERFACE log)
    endif()
endif()
FetchContent_GetProperties(sentencepiece)

#  Library

add_library(vasdk_moss_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    MossVoiceSynthesisProvider.hpp
    MossVoiceSynthesisProvider.cpp
    impl/MossTTSImpl.hpp
    impl/MossTTSImpl.cpp
    impl/MossTextNormalizer.hpp
    impl/MossTextNormalizer.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_moss_tts
    BASE_NAME VASDK_MOSS_TTS
    EXPORT_FILE_NAME vasdk_moss_tts_export.h
)

target_include_directories(vasdk_moss_tts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/impl
        ${sentencepiece_SOURCE_DIR}/src
)

target_link_libraries(vasdk_moss_tts
    PUBLIC  onnxruntime_lib
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE sentencepiece-static
)

if(MSVC)
    target_compile_options(vasdk_moss_tts PRIVATE /utf-8)
endif()

#  Tests
if(VASDK_BUILD_TESTS)
    # Normalizer: parity with the reference robust normalizer (see tests/golden-gen) plus
    # expected spoken forms of the semantic stage. Needs no model files.
    add_executable(moss_normalize_test tests/normalize_test.cpp impl/MossTextNormalizer.cpp)
    target_include_directories(moss_normalize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/impl)
    target_link_libraries(moss_normalize_test PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(moss_normalize_test PRIVATE /utf-8)
    endif()
    add_test(NAME moss_normalize
             COMMAND moss_normalize_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/robust_golden.jsonl)

    # End-to-end synthesis smoke test; reports "skipped" when the models have not been downloaded.
    add_executable(moss_tts_synth_test tests/synth_test.cpp)
    target_link_libraries(moss_tts_synth_test PRIVATE vasdk_moss_tts)
    add_test(NAME moss_tts_synth
             COMMAND moss_tts_synth_test ${MOSS_TTS_MODEL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/moss_tts_test.wav)
    set_tests_properties(moss_tts_synth PROPERTIES SKIP_RETURN_CODE 77)
endif()

#  Install

install(TARGETS vasdk_moss_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES MossVoiceSynthesisProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MossTTS
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_moss_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MossTTS
)
