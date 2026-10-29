
# --- Download the pyannote/speaker-diarization-3.1 embedding model (ONNX) ---
# pyannote/wespeaker-voxceleb-resnet34-LM is PyTorch-only on Hugging Face; this is the same
# WeSpeaker voxceleb ResNet34-LM checkpoint exported to ONNX by sherpa-onnx.
set(PYANNOTE_EMBEDDING_MODEL_URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx")
set(PYANNOTE_EMBEDDING_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/pyannote_embedding.onnx")

if(NOT EXISTS ${PYANNOTE_EMBEDDING_MODEL_DESTINATION})
    message(STATUS "Downloading pyannote embedding ONNX model...")
    file(DOWNLOAD ${PYANNOTE_EMBEDDING_MODEL_URL} ${PYANNOTE_EMBEDDING_MODEL_DESTINATION}
         STATUS download_status
         SHOW_PROGRESS)
    list(GET download_status 0 download_error_code)
    list(GET download_status 1 download_error_string)
    if(NOT download_error_code EQUAL 0)
        file(REMOVE ${PYANNOTE_EMBEDDING_MODEL_DESTINATION})
        message(FATAL_ERROR "Failed to download pyannote embedding ONNX model: ${download_error_string}")
    else()
        message(STATUS "pyannote embedding ONNX model downloaded to ${PYANNOTE_EMBEDDING_MODEL_DESTINATION}")
    endif()
else()
    message(STATUS "pyannote embedding ONNX model already exists at ${PYANNOTE_EMBEDDING_MODEL_DESTINATION}, skipping download.")
endif()
