# --- Supertonic 3 TTS (manifest id supertonic), ONNX, ~400 MB ---
# onnx/ (duration predictor, text encoder, vector estimator, vocoder, tts.json, unicode indexer) and
# voice_styles/ (M1-M5, F1-F5), from supertone-oss-archive/supertonic-3 at its final snapshot.
# Licensed OpenRAIL: the configure prints the license when it downloads.
# -DVASDK_DOWNLOAD_SUPERTONIC_MODELS=OFF skips the download.
option(VASDK_DOWNLOAD_SUPERTONIC_MODELS "Download Supertonic 3 TTS model files" ON)
# Versioned directory: Supertonic 2 lived in models/supertonic
set(SUPERTONIC_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/supertonic3" CACHE PATH "Directory to store Supertonic model files")
if(VASDK_DOWNLOAD_SUPERTONIC_MODELS)
    vasdk_download_model(supertonic DIRECTORY "${SUPERTONIC_MODEL_DIR}" STRIP supertonic3/)
else()
    message(STATUS "Supertonic model download disabled. Place model files in: ${SUPERTONIC_MODEL_DIR}/")
endif()
