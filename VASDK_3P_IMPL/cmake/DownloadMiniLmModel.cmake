# --- all-MiniLM-L6-v2 sentence encoder (manifest id minilm), int8 ONNX for this CPU + vocab ---
set(MINILM_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/minilm")
vasdk_download_model(minilm DIRECTORY "${MINILM_MODEL_DIR}" STRIP minilm/)
