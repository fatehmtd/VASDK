add_library(vasdk_deepgrampp_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    DeepgramppTranscriptionProvider.h DeepgramppTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_deepgrampp_stt
    BASE_NAME VASDK_DEEPGRAMPP_STT
    EXPORT_FILE_NAME vasdk_deepgrampp_stt_export.h
)

target_include_directories(vasdk_deepgrampp_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_deepgrampp_stt
    PUBLIC deepgrampp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_deepgrampp_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES DeepgramppTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/DeepgramppTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_deepgrampp_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/DeepgramppTranscription
)
