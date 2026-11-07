# Fetch pffft for efficient FFT computation
include(FetchContent)

fetchcontent_declare(
    pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG        master
    GIT_SHALLOW    ON
)

fetchcontent_makeavailable(pffft)

add_library(vasdk_vizanim ${VASDK_THIRDPARTY_LIB_TYPE}
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimProvider.h
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimProvider.cpp
    ${CMAKE_CURRENT_LIST_DIR}/impl/VizAnimImpl.h
    ${CMAKE_CURRENT_LIST_DIR}/AudioFeaturesFrame.h
    ${CMAKE_CURRENT_LIST_DIR}/AudioFeaturesEvent.h
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimFrameProcessor.h
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimFrameProcessor.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_vizanim
    BASE_NAME VASDK_VIZANIM
    EXPORT_FILE_NAME vasdk_vizanim_export.h
)

target_include_directories(vasdk_vizanim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${pffft_SOURCE_DIR}
)

target_link_libraries(vasdk_vizanim
    PUBLIC  spdlog::spdlog
    PUBLIC  hwy
    PUBLIC  VASDK
    PRIVATE PFFFT
)

install(TARGETS vasdk_vizanim
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimProvider.h
    ${CMAKE_CURRENT_LIST_DIR}/AudioFeaturesFrame.h
    ${CMAKE_CURRENT_LIST_DIR}/AudioFeaturesEvent.h
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimFrameProcessor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VizAnim
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_vizanim_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VizAnim
)

if(VASDK_BUILD_TESTS)
    add_executable(vasdk_vizanim_test ${CMAKE_CURRENT_LIST_DIR}/tests/vizanim_test.cpp)
    target_link_libraries(vasdk_vizanim_test PRIVATE vasdk_vizanim)
    vasdk_test_dlls(vasdk_vizanim_test)
    add_test(NAME vasdk_vizanim COMMAND vasdk_vizanim_test)
endif()
