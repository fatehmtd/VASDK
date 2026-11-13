# Provider evaluation on real speech (docs/evaluation.md): vasdk_provider_eval measures the local
# providers of the catalog on the evaluation set, writes a report, and holds them to baselines.txtpb
# in ctest (label "eval": ctest -L eval).
#
# The set (model eval_audio_v1 of data/manifest.json, about 37 MB) downloads at configure time with
# VASDK_DOWNLOAD_EVAL_DATA=ON, into VASDK_EVAL_DATA_DIR; without it the eval tests skip (77).

add_library(vasdk_evaluation STATIC
    EvalData.cpp
    Metrics.cpp
    Report.cpp
    Stages.cpp
    SttEval.cpp
    TtsEval.cpp
    VadEval.cpp
    TurnEval.cpp
    SpeakerEval.cpp
    DenoiseEval.cpp
    proto/vasdk/eval/eval_set.proto
)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto)   # protoc doesn't create its output folder
protobuf_generate(TARGET vasdk_evaluation
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto ${VASDK_PROTOBUF_IMPORT_DIRS}
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
target_include_directories(vasdk_evaluation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/proto)
target_link_libraries(vasdk_evaluation PUBLIC vasdk_provider_catalog nlohmann_json::nlohmann_json)

set(VASDK_EVAL_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/data/manifest.json)
add_executable(vasdk_provider_eval main.cpp)
target_link_libraries(vasdk_provider_eval PRIVATE vasdk_evaluation)
target_compile_definitions(vasdk_provider_eval PRIVATE VASDK_EVAL_MANIFEST="${VASDK_EVAL_MANIFEST}")
vasdk_test_dlls(vasdk_provider_eval)

# The evaluation set, at configure time
option(VASDK_DOWNLOAD_EVAL_DATA "Download the provider evaluation set (about 37 MB, CC BY 4.0) at configure time" OFF)
set(VASDK_EVAL_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/../eval/vasdk_eval_v1" CACHE PATH
    "The evaluation set's folder (references.txtpb and clips/), which the eval tests read")
if(VASDK_DOWNLOAD_EVAL_DATA)
    if(EXISTS ${VASDK_EVAL_MANIFEST})
        get_filename_component(_vasdk_eval_store "${VASDK_EVAL_DATA_DIR}" DIRECTORY)
        vasdk_download_model(eval_audio_v1 DIRECTORY "${_vasdk_eval_store}" MANIFEST ${VASDK_EVAL_MANIFEST})
    else()
        message(WARNING "VASDK_DOWNLOAD_EVAL_DATA: no ${VASDK_EVAL_MANIFEST} yet (tools/eval-data/build_eval_set.py writes it)")
    endif()
endif()

# Each local provider built, on the smoke tier, held to its baselines; skipped (77) without the set or its model
set(_vasdk_eval_runs
    # stage    provider       target                 extra arguments
    "stt|whistle|vasdk_whistle_stt|--mode|oneshot"
    "stt|whistle|vasdk_whistle_stt|--mode|streaming"
    "stt|moonshine|vasdk_moonshine_stt|--mode|oneshot"
    "stt|moonshine|vasdk_moonshine_stt|--mode|streaming"
    "stt|parakeet|vasdk_parakeet|--mode|oneshot"
    "stt|parakeet|vasdk_parakeet|--mode|streaming"
    "tts|pocket|vasdk_pocket_tts|"
    "tts|phonon|vasdk_phonon_tts|"
    "tts|supertonic|vasdk_supertonic_tts|"
    "tts|moss|vasdk_moss_tts|"
    "vad|silero|vasdk_silero_vad|"
    "turn|smartturn-v3|vasdk_smartturn_v3|"
    "turn|turn-1-mini|vasdk_turn1mini|"
    "speaker|pyannote|vasdk_pyannote_speaker|"
    "denoise|rnnoise|vasdk_rnnoise|"
)
foreach(_run IN LISTS _vasdk_eval_runs)
    string(REPLACE "|" ";" _fields "${_run}")
    list(REMOVE_ITEM _fields "")
    list(GET _fields 0 _stage)
    list(GET _fields 1 _provider)
    list(GET _fields 2 _target)
    list(LENGTH _fields _count)
    set(_extra "")
    set(_name "vasdk_eval_${_stage}_${_provider}")
    if(_count GREATER 3)
        list(SUBLIST _fields 3 -1 _extra)
        list(GET _extra -1 _mode)
        string(APPEND _name "_${_mode}")
    endif()
    string(REPLACE "-" "_" _name "${_name}")
    if(NOT TARGET ${_target})
        continue()
    endif()
    add_test(NAME ${_name}
             COMMAND vasdk_provider_eval ${_stage} --provider ${_provider} ${_extra} --tier smoke
                     --data ${VASDK_EVAL_DATA_DIR} --models ${CMAKE_CURRENT_BINARY_DIR}/../models
                     --check ${CMAKE_CURRENT_SOURCE_DIR}/baselines.txtpb)
    set_tests_properties(${_name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS eval)
endforeach()

add_executable(vasdk_eval_metrics_test tests/eval_metrics_test.cpp)
target_link_libraries(vasdk_eval_metrics_test PRIVATE vasdk_evaluation)
add_test(NAME vasdk_eval_metrics COMMAND vasdk_eval_metrics_test)
vasdk_label_tests(unit vasdk_eval_metrics)
vasdk_test_dlls(vasdk_eval_metrics_test)
