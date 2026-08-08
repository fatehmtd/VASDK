add_library(vasdk_fishaudiopp_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    FishaudioppVoiceSynthesisProvider.h FishaudioppVoiceSynthesisProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_fishaudiopp_tts
    BASE_NAME VASDK_FISHAUDIOPP_TTS
    EXPORT_FILE_NAME vasdk_fishaudiopp_tts_export.h
)

target_include_directories(vasdk_fishaudiopp_tts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_fishaudiopp_tts
    PUBLIC fishaudiopp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_fishaudiopp_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES FishaudioppVoiceSynthesisProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FishaudioppVoiceSynthesis
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_fishaudiopp_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FishaudioppVoiceSynthesis
)
