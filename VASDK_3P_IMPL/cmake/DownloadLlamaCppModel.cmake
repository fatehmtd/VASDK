
# --- Download llama.cpp GGUF Model ---
# This will download the model during the CMake configuration step if it doesn't exist.
#
# The URL is controlled by the VASDK_LLAMACPP_MODEL_URL cache variable.
# Override it with -DVASDK_LLAMACPP_MODEL_URL=<url> to use a different model.
#
# Default: LFM2.5 1.2B Instruct Q4_K_M (731 MB) from LiquidAI's HuggingFace repo.

set(VASDK_LLAMACPP_MODEL_URL
    "https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/resolve/main/LFM2.5-1.2B-Instruct-Q4_K_M.gguf"
    CACHE STRING "URL of the GGUF model file to download for llama.cpp inference"
)

# Derive the local filename from the URL
get_filename_component(VASDK_LLAMACPP_MODEL_FILENAME "${VASDK_LLAMACPP_MODEL_URL}" NAME)
set(VASDK_LLAMACPP_MODEL_DESTINATION
    "${CMAKE_CURRENT_BINARY_DIR}/models/${VASDK_LLAMACPP_MODEL_FILENAME}"
)

if(VASDK_DOWNLOAD_LLAMACPP_MODEL)
    if(NOT EXISTS "${VASDK_LLAMACPP_MODEL_DESTINATION}")
        message(STATUS "Downloading llama.cpp GGUF model: ${VASDK_LLAMACPP_MODEL_FILENAME}")
        message(STATUS "  From: ${VASDK_LLAMACPP_MODEL_URL}")
        file(DOWNLOAD "${VASDK_LLAMACPP_MODEL_URL}" "${VASDK_LLAMACPP_MODEL_DESTINATION}"
             STATUS download_status
             SHOW_PROGRESS
             TIMEOUT 600)
        list(GET download_status 0 download_error_code)
        list(GET download_status 1 download_error_string)
        if(NOT download_error_code EQUAL 0)
            file(REMOVE "${VASDK_LLAMACPP_MODEL_DESTINATION}")
            message(FATAL_ERROR "Failed to download GGUF model: ${download_error_string}\n"
                                "Check VASDK_LLAMACPP_MODEL_URL and your network connection.")
        else()
            message(STATUS "GGUF model downloaded to ${VASDK_LLAMACPP_MODEL_DESTINATION}")
        endif()
    else()
        message(STATUS "llama.cpp GGUF model already exists at ${VASDK_LLAMACPP_MODEL_DESTINATION}, skipping download.")
    endif()
endif()
