# --- Pocket TTS (manifest id pocket_tts), ONNX int8, ~180 MB ---
# text_conditioner_int8.onnx, flow_lm_main_int8.onnx, flow_lm_flow_int8.onnx, mimi_decoder_int8.onnx,
# tokenizer.model (SentencePiece) and voices.bin (voice embeddings), from the Hugging Face Space
# KevinAHM/pocket-tts-web at the manifest's revision.
# -DVASDK_DOWNLOAD_POCKET_TTS_MODELS=OFF skips the download; -DPOCKET_TTS_MODEL_DIR moves it.
option(VASDK_DOWNLOAD_POCKET_TTS_MODELS
    "Download Pocket TTS model files from HuggingFace during CMake configure" ON)
set(POCKET_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/pocket_tts"
    CACHE PATH "Directory to store Pocket TTS model files")
if(VASDK_DOWNLOAD_POCKET_TTS_MODELS)
    vasdk_download_model(pocket_tts DIRECTORY "${POCKET_TTS_MODEL_DIR}" STRIP pocket_tts/)
else()
    message(STATUS "[PocketTTS] Model download disabled. Place model files in: ${POCKET_TTS_MODEL_DIR}/")
endif()
set(POCKET_TTS_MODEL_DIR "${POCKET_TTS_MODEL_DIR}" PARENT_SCOPE)
