# --- Download whisper.cpp GGML Model ---
#
# Downloads a quantized Whisper GGML model from HuggingFace.
# Source: https://huggingface.co/ggerganov/whisper.cpp
#
# Available models (set -DWHISPERCPP_MODEL=<name>):
#   ggml-tiny.en.bin     (~75  MB)  - English-only, fastest
#   ggml-tiny.bin        (~75  MB)  - Multilingual tiny
#   ggml-base.en.bin     (~142 MB)  - English-only, good balance  [default]
#   ggml-base.bin        (~142 MB)  - Multilingual base
#   ggml-small.en.bin    (~466 MB)  - English-only, higher accuracy
#   ggml-small.bin       (~466 MB)  - Multilingual small
#   ggml-medium.en.bin   (~1.5 GB)  - English-only, high accuracy
#   ggml-medium.bin      (~1.5 GB)  - Multilingual medium
#   ggml-large-v3.bin    (~3.1 GB)  - Multilingual, best accuracy
#
# Files are downloaded to ${WHISPERCPP_MODEL_DIR}.
# Set -DVASDK_DOWNLOAD_WHISPERCPP_MODEL=OFF to skip the download.
# Set -DWHISPERCPP_HF_REPO to override the HuggingFace repository.

option(VASDK_DOWNLOAD_WHISPERCPP_MODEL "Download whisper.cpp GGML model file" ON)

set(WHISPERCPP_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/whispercpp"
    CACHE PATH "Directory to store the whisper.cpp GGML model file")

set(WHISPERCPP_MODEL
    "ggml-medium.bin"
    CACHE STRING "whisper.cpp GGML model filename to download")

set(WHISPERCPP_HF_REPO
    "ggerganov/whisper.cpp"
    CACHE STRING "HuggingFace repository for whisper.cpp models")

set(WHISPERCPP_HF_BASE "https://huggingface.co/${WHISPERCPP_HF_REPO}/resolve/main")

file(MAKE_DIRECTORY "${WHISPERCPP_MODEL_DIR}")

if(VASDK_DOWNLOAD_WHISPERCPP_MODEL)
    set(DEST "${WHISPERCPP_MODEL_DIR}/${WHISPERCPP_MODEL}")
    if(NOT EXISTS "${DEST}")
        set(URL "${WHISPERCPP_HF_BASE}/${WHISPERCPP_MODEL}")
        message(STATUS "Downloading whisper.cpp model '${WHISPERCPP_MODEL}' from ${URL}")
        file(DOWNLOAD "${URL}" "${DEST}"
             STATUS   download_status
             SHOW_PROGRESS
             TIMEOUT  600)
        list(GET download_status 0 error_code)
        list(GET download_status 1 error_string)
        if(NOT error_code EQUAL 0)
            message(WARNING "Failed to download ${WHISPERCPP_MODEL}: ${error_string}")
            file(REMOVE "${DEST}")
        else()
            message(STATUS "whisper.cpp model downloaded to ${DEST}")
        endif()
    else()
        message(STATUS "whisper.cpp model '${WHISPERCPP_MODEL}' already exists, skipping download.")
    endif()
else()
    message(STATUS "whisper.cpp model download disabled. Place a GGML model file in: ${WHISPERCPP_MODEL_DIR}/")
endif()
