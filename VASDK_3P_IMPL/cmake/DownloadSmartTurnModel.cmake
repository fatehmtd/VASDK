
# --- Download SmartTurn V3 ONNX Model ---
# This will download the model during the CMake configuration step if it doesn't exist.
set(SMART_TURN_V3_MODEL_URL "https://huggingface.co/pipecat-ai/smart-turn-v3/resolve/main/smart-turn-v3.2-cpu.onnx")
set(SMART_TURN_V3_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/smart_turn_v3.onnx") # Download to build directory

if(NOT EXISTS ${SMART_TURN_V3_MODEL_DESTINATION})
    message(STATUS "Downloading SmartTurn V3 ONNX model...")
    file(REMOVE ${SMART_TURN_V3_MODEL_DESTINATION}  # Remove if it exists to ensure a fresh download
         RESULT_VARIABLE remove_status)
    file(DOWNLOAD ${SMART_TURN_V3_MODEL_URL} ${SMART_TURN_V3_MODEL_DESTINATION}
         STATUS download_status
         SHOW_PROGRESS)
    list(GET download_status 0 download_error_code)
    list(GET download_status 1 download_error_string)
    if(NOT download_error_code EQUAL 0)
        message(FATAL_ERROR "Failed to download SmartTurn V3 ONNX model: ${download_error_string}")
    else()
        message(STATUS "SmartTurn V3 ONNX model downloaded to ${SMART_TURN_V3_MODEL_DESTINATION}")
    endif()
else()
    message(STATUS "SmartTurn V3 ONNX model already exists at ${SMART_TURN_V3_MODEL_DESTINATION}, skipping download.")
endif()

