add_library(vasdk_sonioxpp_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    SonioxppVoiceSynthesisProvider.h
    SonioxppVoiceSynthesisProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_sonioxpp_tts
    BASE_NAME VASDK_SONIOXPP_TTS
    EXPORT_FILE_NAME vasdk_sonioxpp_tts_export.h
)

target_include_directories(vasdk_sonioxpp_tts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${cppcodec_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_sonioxpp_tts
    PUBLIC sonioxpp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

if(APPLE AND EXISTS "/opt/homebrew/lib")
    target_link_directories(vasdk_sonioxpp_tts PUBLIC /opt/homebrew/lib)
endif()

install(TARGETS vasdk_sonioxpp_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SonioxppVoiceSynthesisProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SonioxppVoiceSynthesis
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_sonioxpp_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SonioxppVoiceSynthesis
)
