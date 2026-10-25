#  ggml
# Reuse the ggml llama.cpp / whisper.cpp already build so the binary links one ggml;
# fetch a pinned release (static) only when neither is enabled.
if(NOT TARGET ggml)
    set(GGML_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(ANDROID)
        # Arm KleidiAI GEMM kernels: much faster Q8_0 matmuls on Cortex CPUs.
        set(GGML_CPU_KLEIDIAI ON CACHE BOOL "" FORCE)
    endif()
    # Static ggml (no DLLs to ship), without touching the cache value other providers see.
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
        ggml
        GIT_REPOSITORY https://github.com/ggml-org/ggml.git
        GIT_TAG        v0.27.0
        GIT_SHALLOW    ON
    )
    FetchContent_MakeAvailable(ggml)
endif()

# Phonon checkpoints have tensor names longer than ggml's default 64 characters.
# GGML_MAX_NAME sizes ggml_tensor::name, so ggml and everything that includes ggml.h
# must agree on it: put it on ggml-base as a PUBLIC definition, which reaches ggml
# itself and every consumer (llama.cpp included), instead of on this target alone.
target_compile_definitions(ggml-base PUBLIC GGML_MAX_NAME=128)

#  SentencePiece
# Shared with PocketTTS when both are enabled.
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

add_library(vasdk_phonon_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    PhononVoiceSynthesisProvider.hpp
    PhononVoiceSynthesisProvider.cpp
    impl/phonon.h
    impl/phonon.cpp
    impl/mimi.h
    impl/mimi.cpp
    impl/model_types.h
    impl/ops.h
    impl/normalize.h
    impl/normalize.cpp
    impl/normalize_tables.inc
    impl/unicode_props.inc
    impl/chunk.h
    impl/chunk.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_phonon_tts
    BASE_NAME VASDK_PHONON_TTS
    EXPORT_FILE_NAME vasdk_phonon_tts_export.h
)

target_include_directories(vasdk_phonon_tts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/impl
        ${sentencepiece_SOURCE_DIR}/src
)

target_link_libraries(vasdk_phonon_tts
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
    PRIVATE ggml
    PRIVATE sentencepiece-static
)

# The engine's conv/attention loops are plain C++; let the compiler vectorise them.
if(MSVC)
    # /utf-8: the normalizer spells non-ASCII characters as literals.
    target_compile_options(vasdk_phonon_tts PRIVATE /utf-8 /fp:fast $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(vasdk_phonon_tts PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

#  Tests
# Parity of the text normalizer with the reference (see tests/golden-gen/README.md).
if(VASDK_BUILD_TESTS)
    add_executable(phonon_normalize_test tests/normalize_test.cpp impl/normalize.cpp)
    target_include_directories(phonon_normalize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/impl)
    target_link_libraries(phonon_normalize_test PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(phonon_normalize_test PRIVATE /utf-8)
    endif()
    add_test(NAME phonon_normalize_parity
             COMMAND phonon_normalize_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/normalize_golden.jsonl)

    # Parity of the text planner (sentence grouping and cutting). The golden half needs the
    # checkpoint's tokenizer.model and reports "skipped" when it has not been downloaded.
    add_executable(phonon_chunk_test tests/chunk_test.cpp impl/chunk.cpp impl/normalize.cpp)
    target_include_directories(phonon_chunk_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/impl ${sentencepiece_SOURCE_DIR}/src)
    target_link_libraries(phonon_chunk_test PRIVATE nlohmann_json::nlohmann_json sentencepiece-static)
    if(MSVC)
        target_compile_options(phonon_chunk_test PRIVATE /utf-8)
    endif()
    add_test(NAME phonon_chunk_unit COMMAND phonon_chunk_test)
    add_test(NAME phonon_chunk_parity
             COMMAND phonon_chunk_test ${PHONON_TTS_MODEL_DIR}/tokenizer.model
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/chunk_golden.jsonl)
    set_tests_properties(phonon_chunk_parity PROPERTIES SKIP_RETURN_CODE 77)
endif()

#  Install

install(TARGETS vasdk_phonon_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES PhononVoiceSynthesisProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PhononTTS
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_phonon_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PhononTTS
)
