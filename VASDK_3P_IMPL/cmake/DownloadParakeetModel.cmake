# --- Parakeet TDT 0.6B v3 (manifest id parakeet), ONNX int8, ~670 MB ---
# encoder-model.int8.onnx, decoder_joint-model.int8.onnx, nemo128.onnx (mel preprocessor) and
# vocab.txt, from istupakov/parakeet-tdt-0.6b-v3-onnx at the manifest's revision.
# -DVASDK_DOWNLOAD_PARAKEET_MODEL=OFF skips the download.
option(VASDK_DOWNLOAD_PARAKEET_MODEL "Download Parakeet ONNX model files" ON)
set(PARAKEET_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/parakeet" CACHE PATH "Directory to store Parakeet model files")
if(VASDK_DOWNLOAD_PARAKEET_MODEL)
    vasdk_download_model(parakeet DIRECTORY "${PARAKEET_MODEL_DIR}" STRIP parakeet/)
else()
    message(STATUS "Parakeet model download disabled. Place model files in: ${PARAKEET_MODEL_DIR}/")
endif()
