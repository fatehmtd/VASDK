add_library(vasdk_miniaudio_audio ${VASDK_THIRDPARTY_LIB_TYPE}
    miniaudio_impl.cpp
    MiniAudioSource.h MiniAudioSource.cpp
    MiniAudioSink.h   MiniAudioSink.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_miniaudio_audio
    BASE_NAME VASDK_MINIAUDIO
    EXPORT_FILE_NAME vasdk_miniaudio_export.h
)

target_include_directories(vasdk_miniaudio_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${miniaudio_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_miniaudio_audio
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
)

# Platform-specific audio back-end libraries required by miniaudio
if(APPLE)
    target_link_libraries(vasdk_miniaudio_audio PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(vasdk_miniaudio_audio PRIVATE pthread dl m)
elseif(WIN32)
    target_link_libraries(vasdk_miniaudio_audio PRIVATE winmm ole32)
endif()

install(TARGETS vasdk_miniaudio_audio
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES MiniAudioSource.h MiniAudioSink.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MiniAudio
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_miniaudio_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/MiniAudio
)
