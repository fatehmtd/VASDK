add_library(vasdk_deepgrampp_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    DeepgramppVoiceSynthesisProvider.h DeepgramppVoiceSynthesisProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_deepgrampp_tts
    BASE_NAME VASDK_DEEPGRAMPP_TTS
    EXPORT_FILE_NAME vasdk_deepgrampp_tts_export.h
)

target_include_directories(vasdk_deepgrampp_tts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_deepgrampp_tts
    PUBLIC deepgrampp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_deepgrampp_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES DeepgramppVoiceSynthesisProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/DeepgramppVoiceSynthesis
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_deepgrampp_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/DeepgramppVoiceSynthesis
)
