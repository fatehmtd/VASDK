add_library(vasdk_cartesiapp_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    CartesiappTranscriptionProvider.h CartesiappTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_cartesiapp_stt
    BASE_NAME VASDK_CARTESIAPP_STT
    EXPORT_FILE_NAME vasdk_cartesiapp_stt_export.h
)

target_include_directories(vasdk_cartesiapp_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_cartesiapp_stt
    PUBLIC cartesiapp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_cartesiapp_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES CartesiappTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CartesiappTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_cartesiapp_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CartesiappTranscription
)
