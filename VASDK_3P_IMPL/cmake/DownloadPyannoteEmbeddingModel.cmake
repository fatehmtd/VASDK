# --- pyannote speaker embedding model (manifest id pyannote_embedding), at configure time ---
# pyannote/wespeaker-voxceleb-resnet34-LM is PyTorch-only on Hugging Face; the manifest names the
# same WeSpeaker checkpoint exported to ONNX by sherpa-onnx.
set(PYANNOTE_EMBEDDING_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/pyannote_embedding.onnx")
vasdk_download_model(pyannote_embedding DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models" REQUIRED)
