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
    ${CMAKE_CURRENT_LIST_DIR}/VizAnimFrameProcessor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VizAnim
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_vizanim_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VizAnim
)
