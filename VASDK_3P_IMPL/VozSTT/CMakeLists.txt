add_library(vasdk_voz_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    VozTranscriptionProvider.hpp
    VozTranscriptionProvider.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_voz_stt
    BASE_NAME VASDK_VOZ_STT
    EXPORT_FILE_NAME vasdk_voz_stt_export.h
)

target_include_directories(vasdk_voz_stt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_voz_stt
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

if(VASDK_BUILD_TESTS)
    add_executable(voz_stt_provider_test tests/provider_test.cpp)
    target_link_libraries(voz_stt_provider_test PRIVATE vasdk_voz_stt)
    add_test(NAME voz_stt_provider COMMAND voz_stt_provider_test)
endif()

include(GNUInstallDirs)

install(TARGETS vasdk_voz_stt COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES VozTranscriptionProvider.hpp ${CMAKE_CURRENT_BINARY_DIR}/vasdk_voz_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/VozSTT
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)
