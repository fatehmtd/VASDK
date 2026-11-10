# --- turn-1-mini model (manifest id turn_1_mini), at configure time: VASDKModels.cmake ---
set(TURN1MINI_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/turn_1_mini.onnx")
if(VASDK_DOWNLOAD_TURN1MINI_MODEL)
    vasdk_download_model(turn_1_mini DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models" REQUIRED)
endif()
