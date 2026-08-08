add_library(vasdk_gradiumpp_tts ${VASDK_THIRDPARTY_LIB_TYPE}
    GradiumppVoiceSynthesisProvider.h GradiumppVoiceSynthesisProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_gradiumpp_tts
    BASE_NAME VASDK_GRADIUMPP_TTS
    EXPORT_FILE_NAME vasdk_gradiumpp_tts_export.h
)

target_include_directories(vasdk_gradiumpp_tts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_gradiumpp_tts
    PUBLIC gradiumpp
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE cppcodec
)

install(TARGETS vasdk_gradiumpp_tts
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES GradiumppVoiceSynthesisProvider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GradiumppVoiceSynthesis
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_gradiumpp_tts_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GradiumppVoiceSynthesis
)
