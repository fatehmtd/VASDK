include(FetchContent)

if(NOT TARGET httplib::httplib)
    message(FATAL_ERROR "VASDK_BUILD_LIBDATACHANNEL_WEBRTC needs cpp-httplib - enable VASDK_ENABLE_WEBSOCKET_SERVER")
endif()

# Dependencies build static and position-independent, folded into this provider library.
# These are normal variables, so they stay in this directory's scope.
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#  libopus
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF)  # opus registers its tests whenever CTest is on, even though they aren't built
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
fetchcontent_declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    ON
    EXCLUDE_FROM_ALL
)
fetchcontent_makeavailable(opus)

#  libdatachannel (ICE via libjuice, DTLS-SRTP via libsrtp, SCTP via usrsctp - all submodules)
# DTLS uses OpenSSL by default; pass -DUSE_MBEDTLS=ON or -DUSE_GNUTLS=ON to use those instead.
# shortcut: on macOS we only probe Homebrew's openssl@3; set OPENSSL_ROOT_DIR for other layouts.
if(APPLE AND NOT OPENSSL_ROOT_DIR AND NOT USE_MBEDTLS AND NOT USE_GNUTLS)
    execute_process(COMMAND brew --prefix openssl@3
                    OUTPUT_VARIABLE _vasdk_brew_openssl OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_vasdk_brew_openssl)
        set(OPENSSL_ROOT_DIR "${_vasdk_brew_openssl}")
    endif()
endif()
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES  ON CACHE BOOL "" FORCE)
set(NO_TESTS     ON CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)  # they need ENABLE_WARNINGS, which other deps turn off
fetchcontent_declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.24.6
    GIT_SHALLOW    ON
    EXCLUDE_FROM_ALL
)
fetchcontent_makeavailable(libdatachannel)

add_library(vasdk_libdatachannel_webrtc ${VASDK_THIRDPARTY_LIB_TYPE}
    LibDataChannelWebRtcProvider.h
    LibDataChannelWebRtcProvider.cpp
    RtpPayload.h
    RtpReorderBuffer.h
)

include(GenerateExportHeader)
generate_export_header(vasdk_libdatachannel_webrtc
    BASE_NAME VASDK_LIBDATACHANNEL_WEBRTC
    EXPORT_FILE_NAME vasdk_libdatachannel_webrtc_export.h
)

target_include_directories(vasdk_libdatachannel_webrtc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vasdk_libdatachannel_webrtc
    PUBLIC  VASDK
    PUBLIC  spdlog::spdlog
    PRIVATE LibDataChannel::LibDataChannelStatic
    PRIVATE Opus::opus
    PRIVATE httplib::httplib
)

#  Tests
if(VASDK_BUILD_TESTS)
    add_executable(vasdk_webrtc_provider_test tests/webrtc_provider_test.cpp)
    target_link_libraries(vasdk_webrtc_provider_test PRIVATE
        vasdk_libdatachannel_webrtc LibDataChannel::LibDataChannelStatic Opus::opus)
    add_test(NAME vasdk_webrtc_provider COMMAND vasdk_webrtc_provider_test)
endif()

install(TARGETS vasdk_libdatachannel_webrtc COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES LibDataChannelWebRtcProvider.h
              ${CMAKE_CURRENT_BINARY_DIR}/vasdk_libdatachannel_webrtc_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/LibDataChannelWebRtc
    COMPONENT vasdk_cpp_development EXCLUDE_FROM_ALL
)
