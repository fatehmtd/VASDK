# Patch supertonic helper.cpp for Windows (ONNX Runtime requires wchar_t* paths)
set(SUPERTONIC_HELPER_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/supertonic_patched/helper.cpp")
file(READ "${supertonic_SOURCE_DIR}/cpp/helper.cpp" _helper_src)
if(WIN32)
    string(REPLACE
        "return std::make_unique<Ort::Session>(env, onnx_path.c_str(), opts);"
        "{ std::wstring wp(onnx_path.begin(), onnx_path.end()); return std::make_unique<Ort::Session>(env, wp.c_str(), opts); }"
        _helper_src "${_helper_src}")
endif()
file(WRITE "${SUPERTONIC_HELPER_PATCHED}" "${_helper_src}")

add_library(vasdk_supertonic_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    SupertonicVoiceSynthesisProvider.hpp SupertonicVoiceSynthesisProvider.cpp
    impl/SupertonicInferenceImpl.hpp impl/SupertonicInferenceImpl.cpp
    ${SUPERTONIC_HELPER_PATCHED}
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_supertonic_tts PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_supertonic_tts
    BASE_NAME VASDK_SUPERTONIC_TTS
    EXPORT_FILE_NAME vasdk_supertonic_tts_export.h
)

target_include_directories(vasdk_supertonic_tts
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${supertonic_SOURCE_DIR}/cpp
)

target_link_libraries(vasdk_supertonic_tts
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_supertonic_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SupertonicVoiceSynthesisProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SupertonicTTS
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_supertonic_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SupertonicTTS
)
