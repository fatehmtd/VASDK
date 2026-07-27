# --- Download Pocket TTS Model Files ---
#
# Downloads ONNX models, SentencePiece tokenizer, and voice embeddings from
# the HuggingFace Space "KevinAHM/pocket-tts-web".
#
# Files downloaded to ${POCKET_TTS_MODEL_DIR} (default: models/pocket_tts/):
#   text_conditioner.onnx     - text tokens → embeddings [1, N, 1024]
#   flow_lm_main_int8.onnx    - autoregressive flow LM (INT8 quantised)
#   flow_lm_flow_int8.onnx    - ODE velocity solver (INT8 quantised)
#   mimi_decoder_int8.onnx    - latents → audio (INT8 quantised)
#   mimi_encoder.onnx         - voice cloning encoder (downloaded only if
#                               POCKET_TTS_DOWNLOAD_MIMI_ENCODER is ON)
#   tokenizer.model           - SentencePiece vocabulary
#   voices.bin                - pre-computed voice embeddings
#
# Usage:
#   cmake -DVASDK_BUILD_POCKET_TTS=ON ..
#   cmake -DVASDK_DOWNLOAD_POCKET_TTS_MODELS=OFF ..   # skip downloads
#   cmake -DPOCKET_TTS_MODEL_DIR=/path/to/models ..   # custom model location

option(VASDK_DOWNLOAD_POCKET_TTS_MODELS
    "Download Pocket TTS model files from HuggingFace during CMake configure" ON)

option(POCKET_TTS_DOWNLOAD_MIMI_ENCODER
    "Also download mimi_encoder.onnx (only needed for runtime voice cloning)" OFF)

set(POCKET_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/pocket_tts"
    CACHE PATH "Directory to store Pocket TTS model files")

set(POCKET_TTS_HF_BASE
    "https://huggingface.co/spaces/KevinAHM/pocket-tts-web/resolve/main/onnx/english_2026-04"
    CACHE STRING "HuggingFace base URL for Pocket TTS ONNX model files")

file(MAKE_DIRECTORY "${POCKET_TTS_MODEL_DIR}")

# ---------------------------------------------------------------------------
# Helper: download a single file from the HF Space, skip if already present.
# Optional second argument: base URL override (default: POCKET_TTS_HF_BASE)
# ---------------------------------------------------------------------------
function(pocket_tts_download FILENAME)
    set(DEST "${POCKET_TTS_MODEL_DIR}/${FILENAME}")
    if(ARGC GREATER 1)
        set(_base "${ARGV1}")
    else()
        set(_base "${POCKET_TTS_HF_BASE}")
    endif()
    if(NOT EXISTS "${DEST}")
        set(URL "${_base}/${FILENAME}")
        message(STATUS "[PocketTTS] Downloading ${FILENAME} ...")
        file(DOWNLOAD "${URL}" "${DEST}"
             STATUS dl_status
             SHOW_PROGRESS
             TIMEOUT 600)
        list(GET dl_status 0 err_code)
        list(GET dl_status 1 err_str)
        if(NOT err_code EQUAL 0)
            message(WARNING "[PocketTTS] Failed to download ${FILENAME}: ${err_str}")
            file(REMOVE "${DEST}")
        else()
            message(STATUS "[PocketTTS] ${FILENAME} saved to ${DEST}")
        endif()
    else()
        message(STATUS "[PocketTTS] ${FILENAME} already present, skipping.")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
if(VASDK_DOWNLOAD_POCKET_TTS_MODELS)
    pocket_tts_download("text_conditioner_int8.onnx")
    pocket_tts_download("flow_lm_main_int8.onnx")
    pocket_tts_download("flow_lm_flow_int8.onnx")
    pocket_tts_download("mimi_decoder_int8.onnx")
    pocket_tts_download("tokenizer.model")
    pocket_tts_download("voices.bin")

    if(POCKET_TTS_DOWNLOAD_MIMI_ENCODER)
        pocket_tts_download("mimi_encoder.onnx")
    endif()
else()
    message(STATUS "[PocketTTS] Model download disabled. "
                   "Place model files in: ${POCKET_TTS_MODEL_DIR}/")
endif()

# Expose the model directory to parent scope so build targets can reference it.
set(POCKET_TTS_MODEL_DIR "${POCKET_TTS_MODEL_DIR}" PARENT_SCOPE)
