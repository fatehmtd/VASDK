add_library(vasdk_gradiumpp_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    GradiumppTranscriptionProvider.h GradiumppTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_gradiumpp_stt
    BASE_NAME VASDK_GRADIUMPP_STT
    EXPORT_FILE_NAME vasdk_gradiumpp_stt_export.h
)

target_include_directories(vasdk_gradiumpp_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_gradiumpp_stt
    PUBLIC gradiumpp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_gradiumpp_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES GradiumppTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GradiumppTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_gradiumpp_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GradiumppTranscription
)
