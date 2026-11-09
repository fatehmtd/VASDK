# --- Smart Turn v3 model (manifest id smart_turn_v3), at configure time: VASDKModels.cmake ---
set(SMART_TURN_V3_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/smart_turn_v3.onnx")
if(VASDK_DOWNLOAD_SMARTTURN_V3_MODEL)
    vasdk_download_model(smart_turn_v3 DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models" REQUIRED)
endif()
