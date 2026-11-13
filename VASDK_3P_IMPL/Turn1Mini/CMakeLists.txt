# turn-1-mini (p99lab): streaming end-of-turn detection on ONNX Runtime, no other dependency
add_library(vasdk_turn1mini ${VASDK_THIRDPARTY_LIB_TYPE}
    Turn1MiniProvider.h Turn1MiniProvider.cpp
    Turn1MiniStream.h Turn1MiniStream.cpp
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_turn1mini PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_turn1mini
    BASE_NAME VASDK_TURN1MINI
    EXPORT_FILE_NAME vasdk_turn1mini_export.h
)

target_include_directories(vasdk_turn1mini PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_turn1mini
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

#  Tests: the stream against the reference's scores, and the provider's decisions (skip without the model)
if(VASDK_BUILD_TESTS)
    if(DEFINED TURN1MINI_MODEL_DESTINATION)
        set(_turn1mini_model ${TURN1MINI_MODEL_DESTINATION})
    else()
        set(_turn1mini_model ${CMAKE_CURRENT_BINARY_DIR}/../models/turn_1_mini.onnx)
    endif()
    add_executable(turn1mini_test tests/turn1mini_test.cpp)
    target_link_libraries(turn1mini_test PRIVATE vasdk_turn1mini)
    vasdk_test_dlls(turn1mini_test)
    add_test(NAME turn1mini_stream
             COMMAND turn1mini_test ${_turn1mini_model} ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/scores_golden.txt)
    vasdk_label_tests(model turn1mini_stream)
    set_tests_properties(turn1mini_stream PROPERTIES SKIP_RETURN_CODE 77)
endif()

install(TARGETS vasdk_turn1mini COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES Turn1MiniProvider.h ${CMAKE_CURRENT_BINARY_DIR}/vasdk_turn1mini_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/Turn1Mini
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)
