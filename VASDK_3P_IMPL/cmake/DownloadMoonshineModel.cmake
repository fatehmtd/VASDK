# --- Download Moonshine Streaming Model Files ---
#
# Downloads one Moonshine streaming model (English) from HuggingFace, pinned to a fixed revision.
# Source: https://huggingface.co/UsefulSensors/moonshine-streaming (MIT)
#
# Files downloaded to ${CMAKE_CURRENT_BINARY_DIR}/models/moonshine/:
#   frontend.ort, encoder.ort, adapter.ort, cross_kv.ort, decoder_kv.ort  - ORT-format graphs
#   streaming_config.json                                                  - model config
#   tokenizer.bin  - binary tokenizer (from download.moonshine.ai; the HF repo only has tokenizer.json),
#                    verified by SHA-256 because that URL is not revision-pinned
#
# Set -DMOONSHINE_MODEL_ARCH=tiny|small|medium to pick the model (default: small).
# Set -DVASDK_DOWNLOAD_MOONSHINE_MODEL=OFF to skip downloads.
# Set -DMOONSHINE_HF_REPO / -DMOONSHINE_HF_REVISION to use a different repo or snapshot.

option(VASDK_DOWNLOAD_MOONSHINE_MODEL "Download Moonshine streaming model files" ON)

set(MOONSHINE_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/moonshine" CACHE PATH "Directory to store Moonshine model files")
set(MOONSHINE_MODEL_ARCH "small" CACHE STRING "Moonshine streaming model size: tiny, small or medium")
set_property(CACHE MOONSHINE_MODEL_ARCH PROPERTY STRINGS tiny small medium)
set(MOONSHINE_HF_REPO "UsefulSensors/moonshine-streaming" CACHE STRING "HuggingFace repo for Moonshine model")
set(MOONSHINE_HF_REVISION "da30bae714913eb0057e733bccb4700104afb1b0" CACHE STRING "HuggingFace revision (commit SHA) for Moonshine model")

if(NOT MOONSHINE_MODEL_ARCH MATCHES "^(tiny|small|medium)$")
    message(FATAL_ERROR "MOONSHINE_MODEL_ARCH must be tiny, small or medium (got '${MOONSHINE_MODEL_ARCH}')")
endif()

set(MOONSHINE_HF_BASE "https://huggingface.co/${MOONSHINE_HF_REPO}/resolve/${MOONSHINE_HF_REVISION}/onnx/${MOONSHINE_MODEL_ARCH}")
set(MOONSHINE_TOKENIZER_URL "https://download.moonshine.ai/model/${MOONSHINE_MODEL_ARCH}-streaming-en/quantized/tokenizer.bin")
set(MOONSHINE_TOKENIZER_SHA256 "6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d")
set(_moonshine_graphs frontend.ort encoder.ort adapter.ort cross_kv.ort decoder_kv.ort streaming_config.json)

file(MAKE_DIRECTORY ${MOONSHINE_MODEL_DIR})

# Helper: download URL to MOONSHINE_MODEL_DIR/FILENAME if it doesn't exist (optional SHA-256 as 3rd arg)
function(moonshine_download URL FILENAME)
    set(DEST "${MOONSHINE_MODEL_DIR}/${FILENAME}")
    if(NOT EXISTS ${DEST})
        set(_hash_args)
        if(ARGC GREATER 2)
            set(_hash_args EXPECTED_HASH SHA256=${ARGV2})
        endif()
        message(STATUS "Downloading ${FILENAME} from ${URL}")
        file(DOWNLOAD ${URL} ${DEST}
             ${_hash_args}
             STATUS download_status
             SHOW_PROGRESS
             TIMEOUT 600)
        list(GET download_status 0 error_code)
        list(GET download_status 1 error_string)
        if(NOT error_code EQUAL 0)
            message(WARNING "Failed to download ${FILENAME}: ${error_string}")
            file(REMOVE ${DEST})
        else()
            message(STATUS "${FILENAME} downloaded to ${DEST}")
        endif()
    else()
        message(STATUS "${FILENAME} already exists, skipping download.")
    endif()
endfunction()

if(VASDK_DOWNLOAD_MOONSHINE_MODEL)
    # Downloads skip existing files, so drop the known model files when the source changes (arch
    # switch, or the pre-streaming base model whose tokenizer.bin is incompatible).
    set(_stamp "${MOONSHINE_HF_REPO}@${MOONSHINE_HF_REVISION}/${MOONSHINE_MODEL_ARCH}")
    set(_stamp_file "${MOONSHINE_MODEL_DIR}/.source")
    set(_old_stamp "")
    if(EXISTS "${_stamp_file}")
        file(READ "${_stamp_file}" _old_stamp)
    endif()
    if(NOT _old_stamp STREQUAL _stamp)
        foreach(_f ${_moonshine_graphs} tokenizer.bin encoder_model.onnx decoder_model_merged.onnx)
            file(REMOVE "${MOONSHINE_MODEL_DIR}/${_f}")
        endforeach()
    endif()

    foreach(_f ${_moonshine_graphs})
        moonshine_download("${MOONSHINE_HF_BASE}/${_f}" "${_f}")
    endforeach()
    moonshine_download("${MOONSHINE_TOKENIZER_URL}" "tokenizer.bin" "${MOONSHINE_TOKENIZER_SHA256}")
    file(WRITE "${_stamp_file}" "${_stamp}")
else()
    message(STATUS "Moonshine model download disabled. Place model files in: ${MOONSHINE_MODEL_DIR}/")
endif()
