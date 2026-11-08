# Flavors: which providers a build has, by name (docs/integrating.md, "Flavors"). The release
# archives and the packages are built in one of them; a source build picks its own with the
# VASDK_BUILD_<NAME> switches, which also refine a flavor (-DVASDK_FLAVOR=default -DVASDK_BUILD_LLAMACPP=ON).
#
#   core      the SDK and the C API, no provider: the app brings its own (vasdk_providers_add_*)
#   default   on-device voice for every platform: miniaudio, Silero VAD, SmartTurn, Whistle STT,
#             Pocket TTS, MiniLM, pyannote, RNNoise, VizAnim (permissive licenses only)
#   full      default, and local LLM and the larger models (llama.cpp, whisper.cpp, Parakeet,
#             Moonshine, Supertonic, MOSS, Phonon, GLiNER) and the cloud STT and TTS clients
#
# A flavor gives the switches their defaults: on a fresh build directory, a switch given on the
# command line wins, and the cache keeps the result (a new flavor needs a new build directory).

set(VASDK_FLAVOR "" CACHE STRING "Provider set: core, default or full (empty: each VASDK_BUILD_<NAME> switch's own default)")
set_property(CACHE VASDK_FLAVOR PROPERTY STRINGS "" core default full)

if(VASDK_FLAVOR STREQUAL "")
    return()
endif()
if(NOT VASDK_FLAVOR MATCHES "^(core|default|full)$")
    message(FATAL_ERROR "VASDK_FLAVOR is '${VASDK_FLAVOR}': core, default or full")
endif()

set(_vasdk_flavor_default
    MINIAUDIO SILERO_VAD ENERGY_ZCR_VAD SMART_TURN_V3 MINILM_NLU PYANNOTE_SPEAKER WHISTLE_STT POCKET_TTS RNNOISE VIZANIM)
set(_vasdk_flavor_full ${_vasdk_flavor_default}
    PHONON_TTS LLAMACPP WHISPERCPP PARAKEET MOONSHINE_STT SUPERTONIC_TTS MOSS_TTS GLINER_NER
    DEEPGRAMPP_STT DEEPGRAMPP_TTS CARTESIAPP_STT CARTESIAPP_TTS FISHAUDIOPP_STT FISHAUDIOPP_TTS
    GRADIUMPP_STT GRADIUMPP_TTS GLADIAPP)
# Every provider switch a flavor decides (the rest keep their own defaults: SDL, FFmpeg, AAudio, Voz,
# libdatachannel and SonioxPP, which need system libraries or a platform SDK)
set(_vasdk_flavor_switches ${_vasdk_flavor_full})

if(VASDK_FLAVOR STREQUAL "core")
    set(_vasdk_flavor_on "")
    if(NOT DEFINED CACHE{VASDK_BUILD_THIRDPARTY_IMPL})
        set(VASDK_BUILD_THIRDPARTY_IMPL OFF CACHE BOOL "Build third-party provider implementations")
    endif()
elseif(VASDK_FLAVOR STREQUAL "default")
    set(_vasdk_flavor_on ${_vasdk_flavor_default})
else()
    set(_vasdk_flavor_on ${_vasdk_flavor_full})
endif()

foreach(_switch IN LISTS _vasdk_flavor_switches)
    if(NOT DEFINED CACHE{VASDK_BUILD_${_switch}})
        if(_switch IN_LIST _vasdk_flavor_on)
            set(VASDK_BUILD_${_switch} ON CACHE BOOL "Build the ${_switch} provider (VASDK_FLAVOR ${VASDK_FLAVOR})")
        else()
            set(VASDK_BUILD_${_switch} OFF CACHE BOOL "Build the ${_switch} provider (VASDK_FLAVOR ${VASDK_FLAVOR})")
        endif()
    endif()
endforeach()
message(STATUS "VASDK: flavor ${VASDK_FLAVOR}")
