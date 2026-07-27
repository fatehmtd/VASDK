add_library(vasdk_gladiapp_transcription ${VASDK_THIRDPARTY_LIB_TYPE}
    GladiaPPTranscriptionProvider.h GladiaPPTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_gladiapp_transcription
    BASE_NAME VASDK_GLADIAPP_TRANSCRIPTION
    EXPORT_FILE_NAME vasdk_gladiapp_transcription_export.h
)

target_include_directories(vasdk_gladiapp_transcription PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_gladiapp_transcription
    PUBLIC gladiapp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_gladiapp_transcription
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES GladiaPPTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GladiappTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_gladiapp_transcription_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GladiappTranscription
)
