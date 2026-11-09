# --- whisper.cpp GGML model: manifest id whispercpp (medium) or whispercpp_<size> ---
# From ggerganov/whisper.cpp at the manifest's revision. -DWHISPERCPP_MODEL=<file> picks one:
#   ggml-tiny.en.bin, ggml-tiny.bin (~75 MB)      ggml-base.en.bin, ggml-base.bin (~142 MB)
#   ggml-small.en.bin, ggml-small.bin (~466 MB)   ggml-medium.en.bin, ggml-medium.bin (~1.5 GB, default)
#   ggml-large-v3.bin (~3.1 GB)
# -DVASDK_DOWNLOAD_WHISPERCPP_MODEL=OFF skips the download; -DWHISPERCPP_MODEL_DIR moves it.
option(VASDK_DOWNLOAD_WHISPERCPP_MODEL "Download whisper.cpp GGML model file" ON)
set(WHISPERCPP_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/whispercpp"
    CACHE PATH "Directory to store the whisper.cpp GGML model file")
set(WHISPERCPP_MODEL "ggml-medium.bin" CACHE STRING "whisper.cpp GGML model filename to download")
set_property(CACHE WHISPERCPP_MODEL PROPERTY STRINGS
    ggml-tiny.en.bin ggml-tiny.bin ggml-base.en.bin ggml-base.bin ggml-small.en.bin ggml-small.bin
    ggml-medium.en.bin ggml-medium.bin ggml-large-v3.bin)
if(VASDK_DOWNLOAD_WHISPERCPP_MODEL)
    if(NOT WHISPERCPP_MODEL MATCHES "^ggml-(.+)\\.bin$")
        message(FATAL_ERROR "WHISPERCPP_MODEL must be ggml-<size>.bin (got '${WHISPERCPP_MODEL}')")
    endif()
    if(CMAKE_MATCH_1 STREQUAL "medium")
        vasdk_download_model(whispercpp DIRECTORY "${WHISPERCPP_MODEL_DIR}" STRIP whispercpp/)
    else()
        vasdk_download_model(whispercpp_${CMAKE_MATCH_1} DIRECTORY "${WHISPERCPP_MODEL_DIR}" STRIP whispercpp/)
    endif()
else()
    message(STATUS "whisper.cpp model download disabled. Place a GGML model file in: ${WHISPERCPP_MODEL_DIR}/")
endif()
