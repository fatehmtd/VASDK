add_library(vasdk_cartesiapp_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    CartesiappVoiceSynthesisProvider.h CartesiappVoiceSynthesisProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_cartesiapp_tts
    BASE_NAME VASDK_CARTESIAPP_TTS
    EXPORT_FILE_NAME vasdk_cartesiapp_tts_export.h
)

target_include_directories(vasdk_cartesiapp_tts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_cartesiapp_tts
    PUBLIC cartesiapp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_cartesiapp_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES CartesiappVoiceSynthesisProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CartesiappVoiceSynthesis
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_cartesiapp_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CartesiappVoiceSynthesis
)
