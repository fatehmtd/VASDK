add_library(vasdk_parakeet ${VASDK_THIRDPARTY_LIB_TYPE}
    ParakeetTranscriptionProvider.hpp ParakeetTranscriptionProvider.cpp
    impl/ParakeetInferenceProviderImpl.hpp
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_parakeet PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_parakeet
    BASE_NAME VASDK_PARAKEET
    EXPORT_FILE_NAME vasdk_parakeet_export.h
)

target_include_directories(vasdk_parakeet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_parakeet
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_parakeet
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ParakeetTranscriptionProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Parakeet
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_parakeet_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Parakeet
)
