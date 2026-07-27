add_library(vasdk_sonioxpp_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    SonioxppTranscriptionProvider.h
    SonioxppTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_sonioxpp_stt
    BASE_NAME VASDK_SONIOXPP_STT
    EXPORT_FILE_NAME vasdk_sonioxpp_stt_export.h
)

target_include_directories(vasdk_sonioxpp_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_sonioxpp_stt
    PUBLIC sonioxpp
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

if(APPLE AND EXISTS "/opt/homebrew/lib")
    target_link_directories(vasdk_sonioxpp_stt PUBLIC /opt/homebrew/lib)
endif()

install(TARGETS vasdk_sonioxpp_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SonioxppTranscriptionProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SonioxppTranscription
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_sonioxpp_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SonioxppTranscription
)
