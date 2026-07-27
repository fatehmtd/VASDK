add_library(vasdk_rnnoise ${VASDK_THIRDPARTY_LIB_TYPE}
    RNNoiseDSPProvider.h
    RNNoiseDSPProvider.cpp
    RNNoiseProcessor.h
    RNNoiseProcessor.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_rnnoise
    BASE_NAME VASDK_RNNOISE
    EXPORT_FILE_NAME vasdk_rnnoise_export.h
)

target_include_directories(vasdk_rnnoise PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_rnnoise
    PUBLIC  rnnoise_lib
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
)

install(TARGETS vasdk_rnnoise
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES RNNoiseDSPProvider.h RNNoiseProcessor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/RNNoise
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_rnnoise_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/RNNoise
)
