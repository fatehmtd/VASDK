
# --- Download Silero VAD ONNX Model ---
# This will download the model during the CMake configuration step if it doesn't exist.
set(SILERO_VAD_MODEL_URL "https://raw.githubusercontent.com/snakers4/silero-vad/refs/heads/master/src/silero_vad/data/silero_vad.onnx")
set(SILERO_VAD_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/silero_vad.onnx") # Download to build directory

if(NOT EXISTS ${SILERO_VAD_MODEL_DESTINATION})
    message(STATUS "Downloading Silero VAD ONNX model...")
    file(REMOVE ${SILERO_VAD_MODEL_DESTINATION}  # Remove if it exists to ensure a fresh download
         RESULT_VARIABLE remove_status)
    file(DOWNLOAD ${SILERO_VAD_MODEL_URL} ${SILERO_VAD_MODEL_DESTINATION}
         STATUS download_status
         SHOW_PROGRESS)
    list(GET download_status 0 download_error_code)
    list(GET download_status 1 download_error_string)
    if(NOT download_error_code EQUAL 0)
        message(FATAL_ERROR "Failed to download Silero VAD ONNX model: ${download_error_string}")
    else()
        message(STATUS "Silero VAD ONNX model downloaded to ${SILERO_VAD_MODEL_DESTINATION}")
    endif()
else()
    message(STATUS "Silero VAD ONNX model already exists at ${SILERO_VAD_MODEL_DESTINATION}, skipping download.")
endif()