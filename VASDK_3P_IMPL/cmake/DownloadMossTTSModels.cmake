# --- MOSS-TTS-Nano (manifest id moss_tts), ONNX, ~710 MB ---
# The MOSS-TTS-Nano 100M export (MOSS-TTS-Nano-100M-ONNX/) and the streaming decoder of its audio
# tokenizer (MOSS-Audio-Tokenizer-Nano-ONNX/), side by side: the first folder's manifest points at
# ../MOSS-Audio-Tokenizer-Nano-ONNX. Both Apache-2.0. The voice-cloning encoder is not downloaded.
# -DVASDK_DOWNLOAD_MOSS_TTS_MODELS=OFF skips the download; -DMOSS_TTS_MODEL_DIR moves it.
option(VASDK_DOWNLOAD_MOSS_TTS_MODELS
    "Download MOSS-TTS-Nano model files from HuggingFace during CMake configure" ON)
set(MOSS_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/moss_tts"
    CACHE PATH "Directory to store MOSS-TTS-Nano model files")
if(VASDK_DOWNLOAD_MOSS_TTS_MODELS)
    vasdk_download_model(moss_tts DIRECTORY "${MOSS_TTS_MODEL_DIR}" STRIP moss_tts/)
else()
    message(STATUS "[MossTTS] Model download disabled. Place model files in: ${MOSS_TTS_MODEL_DIR}/")
endif()
set(MOSS_TTS_MODEL_DIR "${MOSS_TTS_MODEL_DIR}" PARENT_SCOPE)
