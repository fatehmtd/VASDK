# pffft for the fbank FFT - already fetched by SmartTurnV3 when that is enabled
if(NOT TARGET PFFFT)
    include(FetchContent)
    fetchcontent_declare(
        pffft
        GIT_REPOSITORY https://github.com/marton78/pffft.git
        GIT_TAG        master
        GIT_SHALLOW    ON
    )
    fetchcontent_makeavailable(pffft)
endif()

add_library(vasdk_pyannote_speaker ${VASDK_THIRDPARTY_LIB_TYPE}
    PyannoteEmbeddingProvider.h PyannoteEmbeddingProvider.cpp
    KaldiFbank.h
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_pyannote_speaker PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_pyannote_speaker
    BASE_NAME VASDK_PYANNOTE_SPEAKER
    EXPORT_FILE_NAME vasdk_pyannote_speaker_export.h
)

target_include_directories(vasdk_pyannote_speaker PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_pyannote_speaker
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE PFFFT
)

#  Tests
if(VASDK_BUILD_TESTS)
    add_executable(pyannote_fbank_test tests/fbank_test.cpp)
    target_include_directories(pyannote_fbank_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pyannote_fbank_test PRIVATE PFFFT)
    add_test(NAME pyannote_fbank_parity
             COMMAND pyannote_fbank_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/fbank_golden.txt)
endif()

install(TARGETS vasdk_pyannote_speaker
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES PyannoteEmbeddingProvider.h ${CMAKE_CURRENT_BINARY_DIR}/vasdk_pyannote_speaker_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PyannoteSpeaker
)
