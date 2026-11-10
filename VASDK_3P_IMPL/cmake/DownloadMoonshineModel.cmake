# --- Moonshine Streaming (English): manifest id moonshine (small), moonshine_tiny or moonshine_medium ---
# frontend/encoder/adapter/cross_kv/decoder_kv .ort graphs and streaming_config.json from
# UsefulSensors/moonshine-streaming (MIT), and tokenizer.bin from download.moonshine.ai (pinned by
# its SHA-256). The sizes land in the same files, so switching replaces them.
# -DMOONSHINE_MODEL_ARCH=tiny|small|medium picks the size; -DVASDK_DOWNLOAD_MOONSHINE_MODEL=OFF skips it.
option(VASDK_DOWNLOAD_MOONSHINE_MODEL "Download Moonshine streaming model files" ON)
set(MOONSHINE_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/moonshine" CACHE PATH "Directory to store Moonshine model files")
set(MOONSHINE_MODEL_ARCH "small" CACHE STRING "Moonshine streaming model size: tiny, small or medium")
set_property(CACHE MOONSHINE_MODEL_ARCH PROPERTY STRINGS tiny small medium)
if(NOT MOONSHINE_MODEL_ARCH MATCHES "^(tiny|small|medium)$")
    message(FATAL_ERROR "MOONSHINE_MODEL_ARCH must be tiny, small or medium (got '${MOONSHINE_MODEL_ARCH}')")
endif()
if(VASDK_DOWNLOAD_MOONSHINE_MODEL)
    if(MOONSHINE_MODEL_ARCH STREQUAL "small")
        vasdk_download_model(moonshine DIRECTORY "${MOONSHINE_MODEL_DIR}" STRIP moonshine/)
    else()
        vasdk_download_model(moonshine_${MOONSHINE_MODEL_ARCH} DIRECTORY "${MOONSHINE_MODEL_DIR}" STRIP moonshine/)
    endif()
else()
    message(STATUS "Moonshine model download disabled. Place model files in: ${MOONSHINE_MODEL_DIR}/")
endif()
