# --- llama.cpp GGUF model, at configure time (VASDK_DOWNLOAD_LLAMACPP_MODEL, off by default) ---
#
# Default: the manifest's lfm2.5-1.2b-instruct, LFM2.5 1.2B Instruct Q4_K_M (731 MB, LFM Open
# License), pinned by revision and SHA-256. -DVASDK_LLAMACPP_MODEL_URL=<url> downloads another GGUF
# instead: the file is yours to choose, so nothing pins it.

set(VASDK_LLAMACPP_MODEL_URL "" CACHE STRING
    "URL of a GGUF model to download for llama.cpp instead of the manifest's LFM2.5 1.2B (empty: the manifest's)")
set(_llamacpp_manifest_model "lfm2.5-1.2b-instruct")
set(_llamacpp_manifest_file "LFM2.5-1.2B-Instruct-Q4_K_M.gguf")
# The URL earlier builds stored as the default: the manifest's model, now pinned
if(VASDK_LLAMACPP_MODEL_URL STREQUAL "https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF/resolve/main/LFM2.5-1.2B-Instruct-Q4_K_M.gguf")
    set(VASDK_LLAMACPP_MODEL_URL "" CACHE STRING "" FORCE)
endif()

if(VASDK_LLAMACPP_MODEL_URL STREQUAL "")
    set(VASDK_LLAMACPP_MODEL_FILENAME "${_llamacpp_manifest_file}")
else()
    get_filename_component(VASDK_LLAMACPP_MODEL_FILENAME "${VASDK_LLAMACPP_MODEL_URL}" NAME)
endif()
set(VASDK_LLAMACPP_MODEL_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/models/${VASDK_LLAMACPP_MODEL_FILENAME}")

if(VASDK_DOWNLOAD_LLAMACPP_MODEL)
    if(VASDK_LLAMACPP_MODEL_URL STREQUAL "")
        vasdk_download_model(${_llamacpp_manifest_model} DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/models" REQUIRED)
    elseif(NOT EXISTS "${VASDK_LLAMACPP_MODEL_DESTINATION}")
        message(STATUS "Downloading llama.cpp GGUF model ${VASDK_LLAMACPP_MODEL_FILENAME} from ${VASDK_LLAMACPP_MODEL_URL}")
        file(DOWNLOAD "${VASDK_LLAMACPP_MODEL_URL}" "${VASDK_LLAMACPP_MODEL_DESTINATION}.part"
             STATUS _llamacpp_status SHOW_PROGRESS INACTIVITY_TIMEOUT 120)
        list(GET _llamacpp_status 0 _llamacpp_code)
        if(NOT _llamacpp_code EQUAL 0)
            file(REMOVE "${VASDK_LLAMACPP_MODEL_DESTINATION}.part")
            message(FATAL_ERROR "Failed to download the GGUF model: ${_llamacpp_status}\n"
                                "Check VASDK_LLAMACPP_MODEL_URL and your network connection.")
        endif()
        file(RENAME "${VASDK_LLAMACPP_MODEL_DESTINATION}.part" "${VASDK_LLAMACPP_MODEL_DESTINATION}")
    endif()
endif()
unset(_llamacpp_manifest_model)
unset(_llamacpp_manifest_file)
