add_library(vasdk_ffmpeg_audio ${VASDK_THIRDPARTY_LIB_TYPE}
    FFmpegAudioSource.h
    FFmpegAudioSource.cpp
    FileTransport.h
    FileTransport.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_ffmpeg_audio
    BASE_NAME VASDK_FFMPEG_AUDIO
    EXPORT_FILE_NAME vasdk_ffmpeg_audio_export.h
)

target_include_directories(vasdk_ffmpeg_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_ffmpeg_audio
    PUBLIC  FFmpeg::FFmpeg
    PUBLIC  spdlog::spdlog
    PUBLIC  VASDK
)

install(TARGETS vasdk_ffmpeg_audio
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES FFmpegAudioSource.h FileTransport.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FFmpegAudio
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_ffmpeg_audio_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/FFmpegAudio
)
