# --- Download Supertonic 3 TTS Model Files ---
#
# Downloads ONNX models and voice styles from HuggingFace, pinned to a fixed revision.
# Source: https://huggingface.co/supertone-oss-archive/supertonic-3
# Upstream is archived (no further releases); the revision pins the final snapshot.
#
# Files downloaded to ${CMAKE_CURRENT_BINARY_DIR}/models/supertonic3/:
#   onnx/duration_predictor.onnx  (~1.5 MB)
#   onnx/text_encoder.onnx        (~27 MB)
#   onnx/vector_estimator.onnx    (~132 MB)
#   onnx/vocoder.onnx             (~101 MB)
#   onnx/tts.json                 (~9 KB)   - Model config
#   onnx/unicode_indexer.json     (~262 KB) - Unicode lookup table
#   voice_styles/M1.json          (~420 KB) - Default male voice style
#
# Set -DVASDK_DOWNLOAD_SUPERTONIC_MODELS=OFF to skip downloads.
# Set -DSUPERTONIC_HF_REPO / -DSUPERTONIC_HF_REVISION to use a different repo or snapshot.
# Set -DSUPERTONIC_VOICE_STYLES to download additional voices (semicolon-separated).
#   Available: M1 M2 M3 M4 M5 F1 F2 F3 F4 F5

option(VASDK_DOWNLOAD_SUPERTONIC_MODELS "Download Supertonic 3 TTS model files" ON)

# Versioned dir name: the downloader skips existing files, so reusing "supertonic" would keep stale v2 weights.
set(SUPERTONIC_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/supertonic3" CACHE PATH "Directory to store Supertonic model files")
set(SUPERTONIC_HF_REPO "supertone-oss-archive/supertonic-3" CACHE STRING "HuggingFace repo for Supertonic model")
set(SUPERTONIC_HF_REVISION "aafc6e32416a594460b32413efc49d7fe4ce6d46" CACHE STRING "HuggingFace revision (commit SHA) for Supertonic model")
set(SUPERTONIC_HF_BASE "https://huggingface.co/${SUPERTONIC_HF_REPO}/resolve/${SUPERTONIC_HF_REVISION}")
set(SUPERTONIC_VOICE_STYLES "M1" CACHE STRING "Voice styles to download (semicolon-separated, e.g. M1;F1;M2)")

file(MAKE_DIRECTORY "${SUPERTONIC_MODEL_DIR}/onnx")
file(MAKE_DIRECTORY "${SUPERTONIC_MODEL_DIR}/voice_styles")

# Helper: download a file from HuggingFace if it doesn't exist
function(supertonic_download SUBPATH)
    set(DEST "${SUPERTONIC_MODEL_DIR}/${SUBPATH}")
    if(NOT EXISTS "${DEST}")
        set(URL "${SUPERTONIC_HF_BASE}/${SUBPATH}")
        message(STATUS "Downloading ${SUBPATH} from ${URL}")
        file(DOWNLOAD "${URL}" "${DEST}"
             STATUS download_status
             SHOW_PROGRESS
             TIMEOUT 600)
        list(GET download_status 0 error_code)
        list(GET download_status 1 error_string)
        if(NOT error_code EQUAL 0)
            message(WARNING "Failed to download ${SUBPATH}: ${error_string}")
            file(REMOVE "${DEST}")
        else()
            message(STATUS "${SUBPATH} downloaded to ${DEST}")
        endif()
    else()
        message(STATUS "${SUBPATH} already exists, skipping download.")
    endif()
endfunction()

if(VASDK_DOWNLOAD_SUPERTONIC_MODELS)
    # ONNX models and config
    supertonic_download("onnx/tts.json")
    supertonic_download("onnx/unicode_indexer.json")
    supertonic_download("onnx/duration_predictor.onnx")
    supertonic_download("onnx/text_encoder.onnx")
    supertonic_download("onnx/vector_estimator.onnx")
    supertonic_download("onnx/vocoder.onnx")

    # Voice styles
    foreach(STYLE IN LISTS SUPERTONIC_VOICE_STYLES)
        supertonic_download("voice_styles/${STYLE}.json")
    endforeach()
else()
    message(STATUS "Supertonic model download disabled. Place model files in: ${SUPERTONIC_MODEL_DIR}/")
endif()
