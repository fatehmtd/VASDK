add_library(vasdk_silero_vad ${VASDK_THIRDPARTY_LIB_TYPE}
    SileroVAD.h SileroVAD.cpp
    SileroVADImpl.h
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_silero_vad PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_silero_vad
    BASE_NAME VASDK_SILERO_VAD
    EXPORT_FILE_NAME vasdk_silero_vad_export.h
)

target_include_directories(vasdk_silero_vad PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_silero_vad
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_silero_vad
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SileroVAD.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SileroVAD
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_silero_vad_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SileroVAD
)
