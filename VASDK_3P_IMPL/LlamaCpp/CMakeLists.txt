add_library(vasdk_llamacpp ${VASDK_THIRDPARTY_LIB_TYPE}
    LlamaCppTransport.h
    LlamaCppTransport.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_llamacpp
    BASE_NAME VASDK_LLAMACPP
    EXPORT_FILE_NAME vasdk_llamacpp_export.h
)

# VASDK_3P_IMPL/ is on the include path so consumers use:
#   #include "LlamaCpp/LlamaCppTransport.h"
target_include_directories(vasdk_llamacpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_llamacpp
    PUBLIC  VASDK
    PUBLIC  spdlog::spdlog
    PRIVATE llama                        # llama.cpp main library (pulls in ggml transitively)
    PRIVATE nlohmann_json::nlohmann_json
)

install(TARGETS vasdk_llamacpp
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES LlamaCppTransport.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/LlamaCpp
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_llamacpp_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/LlamaCpp
)
