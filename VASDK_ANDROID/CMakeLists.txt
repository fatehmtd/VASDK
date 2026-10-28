cmake_minimum_required(VERSION 3.22)
project(VASDK_Android VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

#  Disable desktop-only features before pulling in VASDK core 
set(VASDK_BUILD_THIRDPARTY_IMPL ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_APP             OFF CACHE BOOL "" FORCE)
set(VASDK_ENABLE_HTTP_TRANSPORT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_LLAMACPP        ON CACHE BOOL "" FORCE)

#  VASDK 3P  
set(VASDK_USE_NNAPI           OFF  CACHE BOOL "" FORCE)
set(VASDK_BUILD_SILERO_VAD ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_SMART_TURN_V3 ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_PARAKEET ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_SUPERTONIC_TTS OFF CACHE BOOL "" FORCE)
set(VASDK_BUILD_POCKET_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_PHONON_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_WHISTLE_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_PYANNOTE_SPEAKER ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_SONIOXPP_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_SONIOXPP_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_DEEPGRAMPP_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_DEEPGRAMPP_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_CARTESIAPP_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_CARTESIAPP_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_FISHAUDIOPP_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_FISHAUDIOPP_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_GRADIUMPP_STT ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_GRADIUMPP_TTS ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_GLADIAPP ON CACHE BOOL "" FORCE)
set(VASDK_BUILD_VIZANIM    ON CACHE BOOL "" FORCE)

#  Shared fetch-content dependencies (same versions as root CMakeLists) 

# Google Highway - required by VizAnim (root CMakeLists fetches this for desktop
# but Android has its own top-level CMake entry point, so we fetch it here too).
set(VASDK_ENABLE_HIGHWAY ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG        "1.2.0"
    GIT_SHALLOW    TRUE
)
set(HWY_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(highway)

FetchContent_Declare(
    spdlog
    URL      https://github.com/gabime/spdlog/archive/refs/tags/v1.15.0.tar.gz
    URL_HASH SHA256=9962648c9b4f1a7bbc76fd8d9172555bad1871fdb14ff4f842ef87949682caa5
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    nlohmann_json
    # v3.12.0+ needed: gladiapp's request builders assign std::optional<T>
    # fields directly into a basic_json, which relies on optional support
    # nlohmann/json only added in 3.12 (v3.11.3 has no to_json/from_json for
    # std::optional at all, so that assignment fails to compile).
    URL      https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz
    URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
)
FetchContent_MakeAvailable(nlohmann_json)

#  libcurl (for Android) 
if(VASDK_ENABLE_HTTP_TRANSPORT)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Not HTTP_ONLY: as of curl 8.21, HTTP_ONLY also force-disables
    # WebSockets (CMakeLists.txt sets CURL_DISABLE_WEBSOCKETS ON as a side
    # effect), which breaks gladiapp's curl_ws-based transport for Gladia's
    # wss:// live endpoint (manifests as "Unsupported protocol" at runtime).
    # Disable the same set of protocols individually instead, leaving
    # WebSockets enabled.
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IPFS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)

    # Build mbedTLS in-tree and wire it into curl for HTTPS support on Android.
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG        v3.6.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(mbedtls)

    # curl's FindMbedTLS module accepts these cache variables; use target names
    # so the same build can resolve dependencies without a system install.
    set(MBEDTLS_INCLUDE_DIR ${mbedtls_SOURCE_DIR}/include CACHE PATH "" FORCE)
    set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
    set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
    set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    # 8.12.0+ required: 8.11.x double-closes the threaded resolver's eventfd on
    # 64-bit Android, which aborts the process under fdsan (curl PR #15725).
    FetchContent_Declare(
        curl
        URL      https://github.com/curl/curl/archive/refs/tags/curl-8_21_0.tar.gz
        URL_HASH SHA256=ec753aa6f408a3ca9f0d6d5f7a77417aecd1544db13c03ae5d443612bf367364
    )
    FetchContent_MakeAvailable(curl)

    # Create the alias that VASDK core expects
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS libcurl)
    endif()
endif()

#  VASDK core library 
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../VASDK vasdk_core)

#  VASDK 3rd party implementation
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../VASDK_3P_IMPL vasdk_3p_impl)

# lib/tls/mbedtls/mbedtls-extensions.c (in the from-source libwebsockets pulled
# in by sonioxpp/deepgrampp) calls mbedtls_x509_get_name(), which mbedTLS 3.x
# only declares in its private library/x509_internal.h (not the public
# mbedtls/x509.h this file includes) -- an NDK toolchain default of bare
# -Werror then turns the resulting implicit-declaration warning fatal. The
# symbol still links fine (it's compiled into mbedcrypto/mbedtls regardless of
# header visibility), so just relax this one warning for the whole target
# rather than patching mbedTLS or lws sources. Applied here (after every
# vendor subdirectory that might fetch libwebsockets) instead of inside
# sonioxpp/deepgrampp's own CMakeLists so it's independent of which one
# happens to create the target first.
if(TARGET websockets)
    target_compile_options(websockets PRIVATE -Wno-error=implicit-function-declaration)
endif()

#  Android JNI shared library
add_library(vasdk_android SHARED
    app/src/main/cpp/audio_source/AAudioSource.cpp
    app/src/main/cpp/audio_sink/AAudioSink.cpp
    app/src/main/cpp/JniUtils.cpp
    app/src/main/cpp/SttProviderFactory.cpp
    app/src/main/cpp/TtsProviderFactory.cpp
    app/src/main/cpp/ToolRegistration.cpp
    app/src/main/cpp/VasdkBridgeProcessor.cpp
    app/src/main/cpp/VizAnimSupport.cpp
    app/src/main/cpp/vasdk_jni.cpp
)

target_include_directories(vasdk_android
    PRIVATE app/src/main/cpp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../VASDK/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../VASDK_3P_IMPL
)

# nlohmann_json is linked PRIVATE inside VASDK core, so its include path doesn't
# propagate to consumers that include VASDK public headers (e.g. Tool.h).
# Link it directly here so vasdk_jni.cpp can resolve <nlohmann/json.hpp>.
target_link_libraries(vasdk_android
    PRIVATE VASDK
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE android
    PRIVATE log
    PRIVATE aaudio
)

# Link the Silero VAD provider if it was built
if(VASDK_BUILD_SILERO_VAD AND TARGET vasdk_silero_vad)
    target_link_libraries(vasdk_android PRIVATE vasdk_silero_vad)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_SILERO_VAD)
    message(STATUS "[vasdk_android] Silero VAD enabled")
endif()

# Link the SmartTurn v3 provider if it was built
if(VASDK_BUILD_SMART_TURN_V3 AND TARGET vasdk_smartturn_v3)
    target_link_libraries(vasdk_android PRIVATE vasdk_smartturn_v3)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_SMART_TURN_V3)
    message(STATUS "[vasdk_android] SmartTurn v3 turn detection enabled")
endif()

if(VASDK_BUILD_PARAKEET AND TARGET vasdk_parakeet)
    target_link_libraries(vasdk_android PRIVATE vasdk_parakeet)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_PARAKEET)
    message(STATUS "[vasdk_android] Parakeet transcription enabled")
endif()

if(VASDK_BUILD_SONIOXPP_STT AND TARGET vasdk_sonioxpp_stt)
    target_link_libraries(vasdk_android PRIVATE vasdk_sonioxpp_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_SONIOXPP_STT)
    message(STATUS "[vasdk_android] Soniox STT provider enabled")
endif()

if(VASDK_BUILD_SONIOXPP_TTS AND TARGET vasdk_sonioxpp_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_sonioxpp_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_SONIOXPP_TTS)
    message(STATUS "[vasdk_android] Soniox TTS provider enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_STT AND TARGET vasdk_deepgrampp_stt)
    target_link_libraries(vasdk_android PRIVATE vasdk_deepgrampp_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_DEEPGRAMPP_STT)
    message(STATUS "[vasdk_android] Deepgram STT provider enabled")
endif()

if(VASDK_BUILD_DEEPGRAMPP_TTS AND TARGET vasdk_deepgrampp_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_deepgrampp_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_DEEPGRAMPP_TTS)
    message(STATUS "[vasdk_android] Deepgram TTS provider enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_STT AND TARGET vasdk_cartesiapp_stt)
    target_link_libraries(vasdk_android PRIVATE vasdk_cartesiapp_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_CARTESIAPP_STT)
    message(STATUS "[vasdk_android] Cartesia STT provider enabled")
endif()

if(VASDK_BUILD_CARTESIAPP_TTS AND TARGET vasdk_cartesiapp_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_cartesiapp_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_CARTESIAPP_TTS)
    message(STATUS "[vasdk_android] Cartesia TTS provider enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_STT AND TARGET vasdk_fishaudiopp_stt)
    target_link_libraries(vasdk_android PRIVATE vasdk_fishaudiopp_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_FISHAUDIOPP_STT)
    message(STATUS "[vasdk_android] Fish Audio STT provider enabled")
endif()

if(VASDK_BUILD_FISHAUDIOPP_TTS AND TARGET vasdk_fishaudiopp_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_fishaudiopp_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_FISHAUDIOPP_TTS)
    message(STATUS "[vasdk_android] Fish Audio TTS provider enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_STT AND TARGET vasdk_gradiumpp_stt)
    target_link_libraries(vasdk_android PRIVATE vasdk_gradiumpp_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_GRADIUMPP_STT)
    message(STATUS "[vasdk_android] Gradium STT provider enabled")
endif()

if(VASDK_BUILD_GRADIUMPP_TTS AND TARGET vasdk_gradiumpp_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_gradiumpp_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_GRADIUMPP_TTS)
    message(STATUS "[vasdk_android] Gradium TTS provider enabled")
endif()

if(VASDK_BUILD_GLADIAPP AND TARGET vasdk_gladiapp_transcription)
    target_link_libraries(vasdk_android PRIVATE vasdk_gladiapp_transcription)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_GLADIAPP)
    message(STATUS "[vasdk_android] Gladia STT provider enabled")
endif()

if(VASDK_BUILD_LLAMACPP AND TARGET vasdk_llamacpp)
    target_link_libraries(vasdk_android PRIVATE vasdk_llamacpp)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_LLAMACPP)
    message(STATUS "[vasdk_android] llama.cpp local LLM provider enabled")
endif()

if(VASDK_BUILD_SUPERTONIC_TTS AND TARGET vasdk_supertonic_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_supertonic_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_SUPERTONIC_TTS)
    message(STATUS "[vasdk_android] Supertonic TTS enabled")
endif()

if(VASDK_BUILD_POCKET_TTS AND TARGET vasdk_pocket_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_pocket_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_POCKET_TTS)
    message(STATUS "[vasdk_android] Pocket TTS enabled")
endif()

if(VASDK_BUILD_PHONON_TTS AND TARGET vasdk_phonon_tts)
    target_link_libraries(vasdk_android PRIVATE vasdk_phonon_tts)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_PHONON_TTS)
    message(STATUS "[vasdk_android] Phonon TTS enabled")
endif()

# The needle engine ships as a static lib for arm64/armv7 only (none for the x86_64 emulator).
if(VASDK_BUILD_WHISTLE_STT AND TARGET vasdk_whistle_stt AND WHISTLE_ENGINE_STATIC)
    target_link_libraries(vasdk_android PRIVATE vasdk_whistle_stt)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_WHISTLE_STT)
    message(STATUS "[vasdk_android] Whistle STT enabled")
endif()

if(VASDK_BUILD_PYANNOTE_SPEAKER AND TARGET vasdk_pyannote_speaker)
    target_link_libraries(vasdk_android PRIVATE vasdk_pyannote_speaker)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_PYANNOTE_SPEAKER)
    message(STATUS "[vasdk_android] pyannote speaker tracking enabled")
endif()

if(VASDK_BUILD_VIZANIM AND TARGET vasdk_vizanim)
    target_link_libraries(vasdk_android PRIVATE vasdk_vizanim)
    target_compile_definitions(vasdk_android PRIVATE VASDK_HAS_VIZANIM)
    message(STATUS "[vasdk_android] VizAnim audio features enabled")
endif()
