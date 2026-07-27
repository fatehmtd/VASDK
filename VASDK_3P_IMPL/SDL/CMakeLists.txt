add_library(vasdk_sdl_audio ${VASDK_THIRDPARTY_LIB_TYPE}
    SDLAudioSource.h SDLAudioSource.cpp
    SDLAudioSink.h SDLAudioSink.cpp
)

include(GenerateExportHeader)
generate_export_header(vasdk_sdl_audio
    BASE_NAME VASDK_SDL_AUDIO
    EXPORT_FILE_NAME vasdk_sdl_audio_export.h
)

target_include_directories(vasdk_sdl_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_sdl_audio
    PUBLIC SDL3::SDL3
    PUBLIC spdlog::spdlog
    PUBLIC VASDK
)

install(TARGETS vasdk_sdl_audio
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES SDLAudioSource.h SDLAudioSink.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SDL
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/vasdk_sdl_audio_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SDL
)
