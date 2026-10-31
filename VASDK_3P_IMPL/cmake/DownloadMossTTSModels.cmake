# --- Download MOSS-TTS-Nano Model Files ---
#
# Downloads the MOSS-TTS-Nano 100M ONNX export and the streaming decoder of its companion
# audio tokenizer (MOSS-Audio-Tokenizer-Nano-ONNX). Both are Apache-2.0 and public.
#
# Files downloaded to ${MOSS_TTS_MODEL_DIR} (default: models/moss_tts/, ~710 MB):
#   MOSS-TTS-Nano-100M-ONNX/
#     browser_poc_manifest.json                 - prompt templates + built-in voice prompt codes
#     tts_browser_onnx_meta.json                - graph file names / IO names
#     tokenizer.model                           - SentencePiece text tokenizer
#     moss_tts_prefill.onnx                     - global transformer prefill
#     moss_tts_decode_step.onnx                 - global transformer decode step (KV cache)
#     moss_tts_global_shared.data               - global transformer weights (~440 MB)
#     moss_tts_local_fixed_sampled_frame.onnx   - local transformer + in-graph sampling
#     moss_tts_local_shared.data                - local transformer weights (~230 MB)
#   MOSS-Audio-Tokenizer-Nano-ONNX/
#     codec_browser_onnx_meta.json              - codec config + streaming state layout
#     moss_audio_tokenizer_decode_step.onnx     - streaming codes → 48 kHz audio decoder
#     moss_audio_tokenizer_decode_shared.data   - decoder weights (~44 MB)
#
# The voice-cloning encoder and the alternative local decoder graphs are not downloaded.
#
# Usage:
#   cmake -DVASDK_BUILD_MOSS_TTS=ON ..
#   cmake -DVASDK_DOWNLOAD_MOSS_TTS_MODELS=OFF ..                          # skip downloads
#   cmake -DMOSS_TTS_HF_REPO=OpenMOSS-Team/MOSS-TTS-Nano-100M-ONNX \
#         -DMOSS_TTS_HF_REVISION=main ..                                   # official export
#   cmake -DMOSS_TTS_MODEL_DIR=/path/to/models ..                          # custom model location

option(VASDK_DOWNLOAD_MOSS_TTS_MODELS
    "Download MOSS-TTS-Nano model files from HuggingFace during CMake configure" ON)

set(MOSS_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/moss_tts"
    CACHE PATH "Directory to store MOSS-TTS-Nano model files")

set(MOSS_TTS_HF_REPO "Supbatomic/moss-tts-nano-100m-onnx-model"
    CACHE STRING "HuggingFace repo with the MOSS-TTS-Nano ONNX export")

set(MOSS_TTS_HF_REVISION "ec4afe55ae63d93fa15a1aed3730ccfd38e6bb86"
    CACHE STRING "Commit (or branch) of MOSS_TTS_HF_REPO to download")

set(MOSS_TTS_CODEC_HF_REPO "OpenMOSS-Team/MOSS-Audio-Tokenizer-Nano-ONNX"
    CACHE STRING "HuggingFace repo with the MOSS audio tokenizer ONNX export")

set(MOSS_TTS_CODEC_HF_REVISION "ceff0d0749bfb3fa2d61149794ec6feef0d1e1ae"
    CACHE STRING "Commit (or branch) of MOSS_TTS_CODEC_HF_REPO to download")

# The manifest references the codec as ../MOSS-Audio-Tokenizer-Nano-ONNX, so keep the
# upstream directory names side by side.
set(_moss_tts_dir   "${MOSS_TTS_MODEL_DIR}/MOSS-TTS-Nano-100M-ONNX")
set(_moss_codec_dir "${MOSS_TTS_MODEL_DIR}/MOSS-Audio-Tokenizer-Nano-ONNX")
file(MAKE_DIRECTORY "${_moss_tts_dir}" "${_moss_codec_dir}")

# ---------------------------------------------------------------------------
# Helper: download one file, skip if already present.
# ---------------------------------------------------------------------------
function(moss_tts_download URL DEST)
    get_filename_component(_name "${DEST}" NAME)
    if(EXISTS "${DEST}")
        message(STATUS "[MossTTS] ${_name} already present, skipping.")
        return()
    endif()
    message(STATUS "[MossTTS] Downloading ${_name} ...")
    file(DOWNLOAD "${URL}" "${DEST}.part"
         STATUS dl_status
         SHOW_PROGRESS
         TIMEOUT 1800)
    list(GET dl_status 0 err_code)
    list(GET dl_status 1 err_str)
    if(NOT err_code EQUAL 0)
        message(WARNING "[MossTTS] Failed to download ${_name}: ${err_str}")
        file(REMOVE "${DEST}.part")
    else()
        file(RENAME "${DEST}.part" "${DEST}")
        message(STATUS "[MossTTS] ${_name} saved to ${DEST}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
if(VASDK_DOWNLOAD_MOSS_TTS_MODELS)
    set(_base "https://huggingface.co/${MOSS_TTS_HF_REPO}/resolve/${MOSS_TTS_HF_REVISION}")
    foreach(_file
            browser_poc_manifest.json
            tts_browser_onnx_meta.json
            tokenizer.model
            moss_tts_prefill.onnx
            moss_tts_decode_step.onnx
            moss_tts_global_shared.data
            moss_tts_local_fixed_sampled_frame.onnx
            moss_tts_local_shared.data)
        moss_tts_download("${_base}/${_file}" "${_moss_tts_dir}/${_file}")
    endforeach()

    set(_base "https://huggingface.co/${MOSS_TTS_CODEC_HF_REPO}/resolve/${MOSS_TTS_CODEC_HF_REVISION}")
    foreach(_file
            codec_browser_onnx_meta.json
            moss_audio_tokenizer_decode_step.onnx
            moss_audio_tokenizer_decode_shared.data)
        moss_tts_download("${_base}/${_file}" "${_moss_codec_dir}/${_file}")
    endforeach()
else()
    message(STATUS "[MossTTS] Model download disabled. "
                   "Place model files in: ${MOSS_TTS_MODEL_DIR}/")
endif()

unset(_moss_tts_dir)
unset(_moss_codec_dir)

# Expose the model directory to parent scope so build targets can reference it.
set(MOSS_TTS_MODEL_DIR "${MOSS_TTS_MODEL_DIR}" PARENT_SCOPE)
