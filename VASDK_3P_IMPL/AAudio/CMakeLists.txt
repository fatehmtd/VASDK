# Android's microphone and speaker through AAudio (the NDK's low-latency audio API): the catalog's
# "aaudio" audio_input and audio_output.
add_library(vasdk_aaudio ${VASDK_THIRDPARTY_LIB_TYPE}
    AAudioSource.h AAudioSource.cpp
    AAudioSink.h   AAudioSink.cpp
)

target_include_directories(vasdk_aaudio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(vasdk_aaudio
    PUBLIC  VASDK
    PUBLIC  spdlog::spdlog
    PRIVATE aaudio
)
