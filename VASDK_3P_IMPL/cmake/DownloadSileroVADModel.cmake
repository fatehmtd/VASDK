# --- Silero VAD model (manifest id silero_vad), at configure time: VASDKModels.cmake ---
set(SILERO_VAD_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/silero_vad.onnx")
vasdk_download_model(silero_vad DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models" REQUIRED)
