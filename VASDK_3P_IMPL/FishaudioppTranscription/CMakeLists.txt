add_library(vasdk_fishaudiopp_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    FishaudioppTranscriptionProvider.h FishaudioppTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_fishaudiopp_stt
    BASE_NAME VASDK_FISHAUDIOPP_STT
    EXPORT_FILE_NAME vasdk_fishaudiopp_stt_export.h
)

target_include_directories(vasdk_fishaudiopp_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_fishaudiopp_stt
    PUBLIC fishaudiopp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_fishaudiopp_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES FishaudioppTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FishaudioppTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_fishaudiopp_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FishaudioppTranscription
)
