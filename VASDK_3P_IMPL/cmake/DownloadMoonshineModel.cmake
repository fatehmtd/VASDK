# --- Download Moonshine Base ONNX Model Files ---
#
# Downloads the Moonshine Base (quantized) model from HuggingFace.
# Source: https://huggingface.co/UsefulSensors/moonshine
#
# Files downloaded to ${CMAKE_CURRENT_BINARY_DIR}/models/moonshine/:
#   encoder_model.onnx        (~20.5 MB)  - Moonshine encoder
#   decoder_model_merged.onnx (~42.5 MB)  - Merged decoder with KV-cache support
#   tokenizer.bin             (~242 KB)   - Binary tokenizer vocabulary
#
# Set -DVASDK_DOWNLOAD_MOONSHINE_MODEL=OFF to skip downloads.
# Set -DMOONSHINE_HF_REPO to use a different HuggingFace repo.

option(VASDK_DOWNLOAD_MOONSHINE_MODEL "Download Moonshine ONNX model files" ON)

set(MOONSHINE_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/moonshine" CACHE PATH "Directory to store Moonshine model files")
set(MOONSHINE_HF_REPO "UsefulSensors/moonshine" CACHE STRING "HuggingFace repo for Moonshine model")
set(MOONSHINE_HF_BASE "https://huggingface.co/${MOONSHINE_HF_REPO}/resolve/main/onnx/merged/base/quantized")

file(MAKE_DIRECTORY ${MOONSHINE_MODEL_DIR})

# Helper: download a file from HuggingFace if it doesn't exist
function(moonshine_download FILENAME)
    set(DEST "${MOONSHINE_MODEL_DIR}/${FILENAME}")
    if(NOT EXISTS ${DEST})
        set(URL "${MOONSHINE_HF_BASE}/${FILENAME}")
        message(STATUS "Downloading ${FILENAME} from ${URL}")
        file(DOWNLOAD ${URL} ${DEST}
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
    moonshine_download("tokenizer.bin")
    moonshine_download("encoder_model.onnx")
    moonshine_download("decoder_model_merged.onnx")
else()
    message(STATUS "Moonshine model download disabled. Place model files in: ${MOONSHINE_MODEL_DIR}/")
endif()
