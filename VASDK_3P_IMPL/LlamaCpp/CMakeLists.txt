add_library(vasdk_llamacpp ${VASDK_THIRDPARTY_LIB_TYPE}
    LlamaCppTransport.h
    LlamaCppTransport.cpp
    LlamaCppCompletion.h
    LlamaCppCompletion.cpp
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

install(TARGETS vasdk_llamacpp COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES LlamaCppTransport.h LlamaCppCompletion.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/LlamaCpp
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_llamacpp_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/LlamaCpp
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)

# The completion on a real model: VASDK_LLAMACPP_TEST_MODEL (a GGUF path), else the downloaded one.
if(VASDK_BUILD_TESTS)
    add_executable(vasdk_llamacpp_completion_test llamacpp_completion_test.cpp)
    target_link_libraries(vasdk_llamacpp_completion_test PRIVATE vasdk_llamacpp)
    set(VASDK_LLAMACPP_TEST_MODEL "${VASDK_LLAMACPP_MODEL_DESTINATION}" CACHE FILEPATH "A GGUF model for vasdk_llamacpp_completion")
    add_test(NAME vasdk_llamacpp_completion COMMAND vasdk_llamacpp_completion_test "${VASDK_LLAMACPP_TEST_MODEL}")
    set_tests_properties(vasdk_llamacpp_completion PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    if(WIN32)
        get_target_property(_vasdk_type VASDK TYPE)
        if(_vasdk_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET vasdk_llamacpp_completion_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VASDK> $<TARGET_FILE_DIR:vasdk_llamacpp_completion_test>)
        endif()
    endif()
endif()
