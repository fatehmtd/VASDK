# Fetch pffft for efficient FFT computation
include(FetchContent)

fetchcontent_declare(
    pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        237ffa283b17e5ef88ef8198b465f41b0268011d   # no release tags: a commit, for reproducible builds
    ${VASDK_FETCH_EXCLUDE}   # its own install rules stay out of VASDK's
)
fetchcontent_makeavailable(pffft)

add_library(vasdk_smartturn_v3 ${VASDK_THIRDPARTY_LIB_TYPE}
    SmartTurnV3Provider.h SmartTurnV3Provider.cpp
    SmartTurnV3Impl.h
)

if(VASDK_USE_NNAPI)
    target_compile_definitions(vasdk_smartturn_v3 PRIVATE VASDK_USE_NNAPI)
endif()

include(GenerateExportHeader)
generate_export_header(vasdk_smartturn_v3
    BASE_NAME VASDK_SMARTTURN_V3
    EXPORT_FILE_NAME vasdk_smartturn_v3_export.h
)

target_include_directories(vasdk_smartturn_v3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_smartturn_v3
    PUBLIC onnxruntime_lib
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
    PRIVATE PFFFT
)

#  Tests
if(VASDK_BUILD_TESTS)
    add_executable(smartturn_features_test tests/features_test.cpp)
    target_include_directories(smartturn_features_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(smartturn_features_test PRIVATE onnxruntime_lib spdlog::spdlog VASDK PFFFT)
    vasdk_test_dlls(smartturn_features_test)
    add_test(NAME smartturn_features_parity
             COMMAND smartturn_features_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/features_golden.txt)
    vasdk_label_tests(unit smartturn_features_parity)
endif()

install(TARGETS vasdk_smartturn_v3 COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SmartTurnV3Provider.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SmartTurnV3
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_smartturn_v3_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SmartTurnV3
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)
