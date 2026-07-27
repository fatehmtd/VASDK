add_library(vasdk_whispercpp ${VASDK_THIRDPARTY_LIB_TYPE}
    WhisperCppTranscriptionProvider.hpp
    WhisperCppTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_whispercpp
    BASE_NAME VASDK_WHISPERCPP
    EXPORT_FILE_NAME vasdk_whispercpp_export.h
)

target_include_directories(vasdk_whispercpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_whispercpp
    PUBLIC  whisper
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
)

include(GNUInstallDirs)

install(TARGETS vasdk_whispercpp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES WhisperCppTranscriptionProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WhisperCpp
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_whispercpp_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WhisperCpp
)
