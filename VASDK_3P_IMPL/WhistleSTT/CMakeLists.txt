add_library(vasdk_whistle_stt ${VASDK_THIRDPARTY_LIB_TYPE}
    WhistleTranscriptionProvider.hpp
    WhistleTranscriptionProvider.cpp
    impl/NeedleEngine.hpp
    impl/NeedleEngine.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_whistle_stt
    BASE_NAME VASDK_WHISTLE_STT
    EXPORT_FILE_NAME vasdk_whistle_stt_export.h
)

if(NOT EXISTS "${WHISTLE_ENGINE_DIR}/include/needle.h")
    message(WARNING "[WhistleSTT] needle.h not found in ${WHISTLE_ENGINE_DIR}/include; "
                    "configure with VASDK_DOWNLOAD_WHISTLE_STT_MODEL=ON.")
endif()

target_include_directories(vasdk_whistle_stt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${WHISTLE_ENGINE_DIR}/include
)

target_link_libraries(vasdk_whistle_stt
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
    PRIVATE nlohmann_json::nlohmann_json
)

if(WHISTLE_ENGINE_STATIC)
    # Android: the NDK-built engine links straight in.
    add_library(needle_engine STATIC IMPORTED)
    set_target_properties(needle_engine PROPERTIES IMPORTED_LOCATION "${WHISTLE_ENGINE_DIR}/libneedle.a")
    target_link_libraries(vasdk_whistle_stt PRIVATE needle_engine)
    target_compile_definitions(vasdk_whistle_stt PRIVATE VASDK_WHISTLE_STATIC_ENGINE)
elseif(UNIX)
    target_link_libraries(vasdk_whistle_stt PRIVATE ${CMAKE_DL_LIBS})
endif()

include(GNUInstallDirs)

install(TARGETS vasdk_whistle_stt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES WhistleTranscriptionProvider.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WhistleSTT
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_whistle_stt_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/WhistleSTT
)
