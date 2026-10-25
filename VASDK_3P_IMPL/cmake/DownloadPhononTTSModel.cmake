# --- Download Phonon TTS Model Files ---
#
# Downloads a Phonon checkpoint folder from HuggingFace, laid out the way the PhononTTS provider
# loads it, plus named voices from the gradium/phonon-voices catalog.
#
# Files downloaded to ${PHONON_TTS_MODEL_DIR} (default: models/phonon_tts/):
#   config.json                 - model configuration
#   tokenizer.model             - SentencePiece text tokenizer
#   model.q8.gguf               - q8_0 weights (~75 MB)
#   default-voice.safetensors   - the checkpoint's default voice ("default")
#   voices/<Name>.safetensors   - one per entry in PHONON_TTS_VOICES
#
# The Phonon 40m models are GATED on HuggingFace and licensed CC BY-NC 4.0 (non-commercial).
# Accept the terms on the model page first (approval is automatic), then either export
# HF_TOKEN or run `hf auth login` before configuring. The token is read from the
# environment / HF cache only and is never stored in CMakeCache.txt.
#
# Usage:
#   cmake -DVASDK_BUILD_PHONON_TTS=ON ..
#   cmake -DVASDK_DOWNLOAD_PHONON_TTS_MODEL=OFF ..            # skip downloads
#   cmake -DPHONON_TTS_HF_REPO=gradium/phonon-31m-en ..       # public English-only model
#   cmake -DPHONON_TTS_VOICES="Zoey;Augustin" ..              # voices to fetch
#   cmake -DPHONON_TTS_MODEL_DIR=/path/to/models ..           # custom model location

option(VASDK_DOWNLOAD_PHONON_TTS_MODEL
    "Download Phonon TTS model files from HuggingFace during CMake configure" ON)

set(PHONON_TTS_MODEL_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/models/phonon_tts"
    CACHE PATH "Directory to store Phonon TTS model files")

set(PHONON_TTS_HF_REPO "gradium/phonon-40m-multilingual"
    CACHE STRING "HuggingFace model repo for Phonon TTS")

set(PHONON_TTS_HF_REVISION "1537ce46c64f42903e0d608b132ccb073dac46d2"
    CACHE STRING "Commit (or branch) of PHONON_TTS_HF_REPO to download")

set(PHONON_TTS_VOICES "Easton;Zoey;Augustin;Anton;Marcos;Davi"
    CACHE STRING "Voice names (as listed in gradium/phonon-voices) to download into voices/")

set(PHONON_TTS_VOICES_REVISION "0ee17205f254b5abc43b266c8a0f215565c352f7"
    CACHE STRING "Commit (or branch) of gradium/phonon-voices to download voices from")

file(MAKE_DIRECTORY "${PHONON_TTS_MODEL_DIR}/voices")

# ---------------------------------------------------------------------------
# HF token: $HF_TOKEN, else the file `hf auth login` writes.
# ---------------------------------------------------------------------------
set(_phonon_hf_token "")
if(DEFINED ENV{HF_TOKEN} AND NOT "$ENV{HF_TOKEN}" STREQUAL "")
    set(_phonon_hf_token "$ENV{HF_TOKEN}")
else()
    set(_phonon_token_files "")
    if(DEFINED ENV{HF_HOME})
        list(APPEND _phonon_token_files "$ENV{HF_HOME}/token")
    endif()
    if(DEFINED ENV{USERPROFILE})
        list(APPEND _phonon_token_files "$ENV{USERPROFILE}/.cache/huggingface/token")
    endif()
    if(DEFINED ENV{HOME})
        list(APPEND _phonon_token_files "$ENV{HOME}/.cache/huggingface/token")
    endif()
    foreach(_f ${_phonon_token_files})
        if(EXISTS "${_f}")
            file(READ "${_f}" _phonon_hf_token)
            string(STRIP "${_phonon_hf_token}" _phonon_hf_token)
            break()
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Helper: download one file, skip if already present. Returns success in OUT_OK.
# ---------------------------------------------------------------------------
function(phonon_tts_download URL DEST OUT_OK)
    set(${OUT_OK} TRUE PARENT_SCOPE)
    get_filename_component(_name "${DEST}" NAME)
    if(EXISTS "${DEST}")
        message(STATUS "[PhononTTS] ${_name} already present, skipping.")
        return()
    endif()
    set(_auth "")
    if(NOT _phonon_hf_token STREQUAL "")
        set(_auth HTTPHEADER "Authorization: Bearer ${_phonon_hf_token}")
    endif()
    message(STATUS "[PhononTTS] Downloading ${_name} ...")
    file(DOWNLOAD "${URL}" "${DEST}.part"
         ${_auth}
         STATUS dl_status
         SHOW_PROGRESS
         TIMEOUT 600)
    list(GET dl_status 0 err_code)
    list(GET dl_status 1 err_str)
    if(NOT err_code EQUAL 0)
        message(WARNING "[PhononTTS] Failed to download ${_name}: ${err_str}")
        file(REMOVE "${DEST}.part")
        set(${OUT_OK} FALSE PARENT_SCOPE)
    else()
        file(RENAME "${DEST}.part" "${DEST}")
        message(STATUS "[PhononTTS] ${_name} saved to ${DEST}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------
if(VASDK_DOWNLOAD_PHONON_TTS_MODEL)
    set(_phonon_base "https://huggingface.co/${PHONON_TTS_HF_REPO}/resolve/${PHONON_TTS_HF_REVISION}")
    set(_phonon_all_ok TRUE)
    foreach(_file config.json tokenizer.model model.q8.gguf default-voice.safetensors)
        phonon_tts_download("${_phonon_base}/${_file}" "${PHONON_TTS_MODEL_DIR}/${_file}" _ok)
        if(NOT _ok)
            set(_phonon_all_ok FALSE)
        endif()
    endforeach()

    if(NOT _phonon_all_ok)
        if(_phonon_hf_token STREQUAL "")
            message(WARNING "[PhononTTS] No HuggingFace token found. ${PHONON_TTS_HF_REPO} may be "
                            "gated: accept its terms at https://huggingface.co/${PHONON_TTS_HF_REPO}, "
                            "then export HF_TOKEN (or run `hf auth login`) and re-configure.")
        else()
            message(WARNING "[PhononTTS] Download failed with a token. Check that you accepted the "
                            "terms at https://huggingface.co/${PHONON_TTS_HF_REPO} with that account.")
        endif()
    endif()

    # Voices: the catalog stores files by voice ID; the provider names a voice after its file,
    # so look each name up in voices.jsonl and save it as voices/<Name>.safetensors.
    if(NOT PHONON_TTS_VOICES STREQUAL "")
        set(_voices_base "https://huggingface.co/gradium/phonon-voices/resolve/${PHONON_TTS_VOICES_REVISION}")
        set(_voices_index "${CMAKE_CURRENT_BINARY_DIR}/phonon-voices.jsonl")
        phonon_tts_download("${_voices_base}/voices.jsonl" "${_voices_index}" _ok)
        if(_ok)
            file(READ "${_voices_index}" _voices_jsonl)
            foreach(_voice ${PHONON_TTS_VOICES})
                set(_dest "${PHONON_TTS_MODEL_DIR}/voices/${_voice}.safetensors")
                if(EXISTS "${_dest}")
                    message(STATUS "[PhononTTS] Voice ${_voice} already present, skipping.")
                    continue()
                endif()
                string(REGEX MATCH "\"filename\": \"(voices/[A-Za-z0-9_-]+\\.safetensors)\", \"name\": \"${_voice}\""
                       _match "${_voices_jsonl}")
                if(NOT _match)
                    message(WARNING "[PhononTTS] Voice '${_voice}' not found in gradium/phonon-voices.")
                    continue()
                endif()
                phonon_tts_download("${_voices_base}/${CMAKE_MATCH_1}" "${_dest}" _ok)
            endforeach()
        endif()
    endif()
else()
    message(STATUS "[PhononTTS] Model download disabled. "
                   "Place model files in: ${PHONON_TTS_MODEL_DIR}/")
endif()

unset(_phonon_hf_token)

# Expose the model directory to parent scope so build targets can reference it.
set(PHONON_TTS_MODEL_DIR "${PHONON_TTS_MODEL_DIR}" PARENT_SCOPE)
