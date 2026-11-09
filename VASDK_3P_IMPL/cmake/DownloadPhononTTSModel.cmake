# --- Phonon TTS: manifest id phonon_tts (40m multilingual) or phonon_tts_en (31m English) ---
#
# Files, laid out the way the PhononTTS provider loads them, in ${PHONON_TTS_MODEL_DIR}
# (default: models/phonon_tts/):
#   config.json, tokenizer.model, model.q8.gguf (q8_0 weights), default-voice.safetensors
#   voices/<Name>.safetensors   - Easton, Zoey, Augustin, Anton, Marcos, Davi (gradium/phonon-voices)
#
# PHONON_TTS_MODEL picks the checkpoint:
#   phonon_tts      gradium/phonon-40m-multilingual: GATED on Hugging Face and licensed CC BY-NC 4.0
#                   (non-commercial). Accept the terms on the model page first (approval is
#                   automatic), then export HF_TOKEN or run `hf auth login` before configuring.
#                   The token is read from the environment or the HF cache, never stored in the cache.
#   phonon_tts_en   gradium/phonon-31m-en: public, Apache 2.0, English only.
# Both land in the same files, so switching replaces them. The voices are Apache 2.0 either way.
#
# -DVASDK_DOWNLOAD_PHONON_TTS_MODEL=OFF skips the download; -DPHONON_TTS_MODEL_DIR moves it.
option(VASDK_DOWNLOAD_PHONON_TTS_MODEL
    "Download Phonon TTS model files from HuggingFace during CMake configure" ON)
set(PHONON_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/phonon_tts"
    CACHE PATH "Directory to store Phonon TTS model files")
set(PHONON_TTS_MODEL "phonon_tts" CACHE STRING
    "Phonon checkpoint: phonon_tts (40m multilingual, gated, CC BY-NC 4.0) or phonon_tts_en (31m English, Apache 2.0)")
set_property(CACHE PHONON_TTS_MODEL PROPERTY STRINGS phonon_tts phonon_tts_en)
# The former switch, -DPHONON_TTS_HF_REPO=gradium/phonon-31m-en, still picks the English checkpoint
if(PHONON_TTS_HF_REPO STREQUAL "gradium/phonon-31m-en")
    message(STATUS "[PhononTTS] PHONON_TTS_HF_REPO is replaced by -DPHONON_TTS_MODEL=phonon_tts_en; using phonon_tts_en")
    set(PHONON_TTS_MODEL "phonon_tts_en")
endif()
if(NOT PHONON_TTS_MODEL MATCHES "^(phonon_tts|phonon_tts_en)$")
    message(FATAL_ERROR "PHONON_TTS_MODEL must be phonon_tts or phonon_tts_en (got '${PHONON_TTS_MODEL}')")
endif()
if(VASDK_DOWNLOAD_PHONON_TTS_MODEL)
    vasdk_download_model(${PHONON_TTS_MODEL} DIRECTORY "${PHONON_TTS_MODEL_DIR}" STRIP phonon_tts/)
else()
    message(STATUS "[PhononTTS] Model download disabled. Place model files in: ${PHONON_TTS_MODEL_DIR}/")
endif()
set(PHONON_TTS_MODEL_DIR "${PHONON_TTS_MODEL_DIR}" PARENT_SCOPE)
