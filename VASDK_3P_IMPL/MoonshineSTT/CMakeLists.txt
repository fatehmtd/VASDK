# Moonshine's own streaming-model code, compiled against VASDK's ONNX Runtime (not libmoonshine,
# which ships a second prebuilt ORT). Its debug allocator sets two OrtAllocator callbacks that only
# exist from ORT API 23; guard them so it also builds against older runtimes.
set(_ms_core "${moonshine_core_SOURCE_DIR}/core")
set(MOONSHINE_ALLOCATOR_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/moonshine_patched/moonshine-ort-allocator.cpp")
file(READ "${_ms_core}/ort-utils/moonshine-ort-allocator.cpp" _alloc_src)
string(REPLACE "void *MoonshineAllocOnStream(" "#if ORT_API_VERSION >= 23\nvoid *MoonshineAllocOnStream(" _alloc_src "${_alloc_src}")
string(REPLACE "void friendlySizeString(" "#endif\nvoid friendlySizeString(" _alloc_src "${_alloc_src}")
string(REPLACE
    "  base.GetStats = MoonshineGetStats;\n  base.AllocOnStream = MoonshineAllocOnStream;\n"
    "#if ORT_API_VERSION >= 23\n  base.GetStats = MoonshineGetStats;\n  base.AllocOnStream = MoonshineAllocOnStream;\n#endif\n"
    _alloc_src "${_alloc_src}")
file(WRITE "${MOONSHINE_ALLOCATOR_PATCHED}" "${_alloc_src}")

add_library(vasdk_moonshine_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    MoonshineSttProvider.hpp
    MoonshineSttProvider.cpp
    impl/MoonshineSttProviderImpl.hpp
    ${_ms_core}/moonshine-streaming-model.cpp
    ${_ms_core}/context-biaser.cpp
    ${_ms_core}/word-alignment.cpp
    ${_ms_core}/ort-utils/ort-utils.cpp
    ${_ms_core}/ort-utils/ort-utils-ep.cpp
    ${_ms_core}/ort-utils/moonshine-tensor-view.cpp
    ${_ms_core}/ort-utils/moonshine-tensor.cpp
    ${MOONSHINE_ALLOCATOR_PATCHED}
    ${_ms_core}/bin-tokenizer/bin-tokenizer.cpp
    ${_ms_core}/moonshine-utils/string-utils.cpp
    ${_ms_core}/moonshine-utils/debug-utils.cpp
    ${_ms_core}/moonshine-utils/file-utils.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_moonshine_stt
    BASE_NAME VASDK_MOONSHINE_STT
    EXPORT_FILE_NAME vasdk_moonshine_stt_export.h
)

target_include_directories(vasdk_moonshine_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(vasdk_moonshine_stt PRIVATE
    ${_ms_core}
    ${_ms_core}/ort-utils
    ${_ms_core}/bin-tokenizer
    ${_ms_core}/moonshine-utils
    ${onnxruntime_SOURCE_DIR}/include
)

target_link_libraries(vasdk_moonshine_stt
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

if(VASDK_BUILD_TESTS)
    # End-to-end streaming smoke test. No speech fixture ships with the repo: point
    # MOONSHINE_TEST_WAV at a 16 kHz mono int16 English WAV (and optionally set the words it
    # must contain); otherwise the test reports "skipped".
    set(MOONSHINE_TEST_WAV "" CACHE FILEPATH "16 kHz mono int16 English speech WAV for moonshine_stt_stream")
    set(MOONSHINE_TEST_EXPECTED "" CACHE STRING "Words the Moonshine transcript must contain")
    add_executable(moonshine_stt_stream_test tests/stream_test.cpp)
    target_link_libraries(moonshine_stt_stream_test PRIVATE vasdk_moonshine_stt)
    add_test(NAME moonshine_stt_stream
             COMMAND moonshine_stt_stream_test ${MOONSHINE_MODEL_DIR} "${MOONSHINE_TEST_WAV}" "${MOONSHINE_TEST_EXPECTED}")
    set_tests_properties(moonshine_stt_stream PROPERTIES SKIP_RETURN_CODE 77)
endif()

install(TARGETS vasdk_moonshine_stt COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES MoonshineSttProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MoonshineSTT
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_moonshine_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MoonshineSTT
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)
