# --- Download Parakeet TDT ONNX Model Files ---
#
# Downloads the Parakeet TDT 0.6B v3 model (ONNX, int8 quantized) from HuggingFace.
# Source: https://huggingface.co/istupakov/parakeet-tdt-0.6b-v3-onnx
#
# Files downloaded to ${CMAKE_CURRENT_BINARY_DIR}/models/:
#   encoder-model.int8.onnx       (~652 MB)  - Conformer encoder (int8)
#   decoder_joint-model.int8.onnx (~18 MB)   - TDT decoder + joint network (int8)
#   nemo128.onnx                  (~140 KB)  - Mel spectrogram preprocessor
#   vocab.txt                     (~94 KB)   - SentencePiece BPE vocabulary
#
# Set -DVASDK_DOWNLOAD_PARAKEET_MODEL=OFF to skip downloads.
# Set -DPARAKEET_HF_REPO to use a different HuggingFace repo.

option(VASDK_DOWNLOAD_PARAKEET_MODEL "Download Parakeet ONNX model files" ON)

set(PARAKEET_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/parakeet" CACHE PATH "Directory to store Parakeet model files")
set(PARAKEET_HF_REPO "istupakov/parakeet-tdt-0.6b-v3-onnx" CACHE STRING "HuggingFace repo for Parakeet model")
set(PARAKEET_HF_BASE "https://huggingface.co/${PARAKEET_HF_REPO}/resolve/main")

file(MAKE_DIRECTORY ${PARAKEET_MODEL_DIR})

# Helper: download a file from HuggingFace if it doesn't exist
function(parakeet_download FILENAME)
    set(DEST "${PARAKEET_MODEL_DIR}/${FILENAME}")
    if(NOT EXISTS ${DEST})
        set(URL "${PARAKEET_HF_BASE}/${FILENAME}")
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

if(VASDK_DOWNLOAD_PARAKEET_MODEL)
    parakeet_download("nemo128.onnx")
    parakeet_download("vocab.txt")
    parakeet_download("decoder_joint-model.int8.onnx")
    parakeet_download("encoder-model.int8.onnx")
else()
    message(STATUS "Parakeet model download disabled. Place model files in: ${PARAKEET_MODEL_DIR}/")
endif()