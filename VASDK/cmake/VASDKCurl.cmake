# Provides CURL::libcurl for the desktop build.
#
# Uses an installed libcurl when CMake can find one (system package, Homebrew,
# or a vcpkg toolchain if the caller passes one). Otherwise curl is built from
# source as a static library, so no package manager is needed. On Windows it
# uses Schannel, the OS TLS stack, and needs no OpenSSL. Elsewhere curl picks
# the TLS library it finds (normally OpenSSL).
#
# Set -DVASDK_FETCH_CURL=ON to always build curl from source.
#
# Sets VASDK_CURL_BUNDLED (global) to ON when curl was built from source, so the
# apps know there is no libcurl DLL to copy next to their executables.

if(TARGET CURL::libcurl)
    return()
endif()

option(VASDK_FETCH_CURL "Always build libcurl from source instead of using an installed one" OFF)

if(NOT VASDK_FETCH_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "VASDK: using installed libcurl ${CURL_VERSION_STRING}")
        set(VASDK_CURL_BUNDLED OFF CACHE INTERNAL "")
        return()
    endif()
endif()

message(STATUS "VASDK: building libcurl from source")

include(FetchContent)

# Static, HTTP(S) + WebSockets only, no optional compression or IDN libraries
# to look for. Same curl release as VASDK_ANDROID (8.12.0+ needed there).
set(BUILD_SHARED_LIBS OFF)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CURL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
foreach(_proto LDAP LDAPS TELNET DICT TFTP RTSP POP3 IMAP SMTP GOPHER FILE FTP IPFS MQTT)
    set(CURL_DISABLE_${_proto} ON CACHE BOOL "" FORCE)
endforeach()
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_WINDOWS_SSPI ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    curl
    URL      https://github.com/curl/curl/archive/refs/tags/curl-8_21_0.tar.gz
    URL_HASH SHA256=ec753aa6f408a3ca9f0d6d5f7a77417aecd1544db13c03ae5d443612bf367364
)
FetchContent_MakeAvailable(curl)
unset(BUILD_SHARED_LIBS)

if(NOT TARGET CURL::libcurl)
    add_library(CURL::libcurl ALIAS libcurl_static)
endif()

set(VASDK_CURL_BUNDLED ON CACHE INTERNAL "")
