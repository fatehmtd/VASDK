# --- Download GLiNER (zero-shot entity model), int8 ONNX, pinned by revision and SHA-256 ---
# VASDK_GLINER_MODEL: small (gliner_small-v2.1, English, 183 MB) or multi (gliner_multi-v2.1,
# multilingual, 349 MB). Both Apache-2.0, ONNX exports by onnx-community.
set(VASDK_GLINER_MODEL "small" CACHE STRING "GLiNER model: small (English) or multi (multilingual)")
set_property(CACHE VASDK_GLINER_MODEL PROPERTY STRINGS small multi)
set(GLINER_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/gliner")

if(VASDK_GLINER_MODEL STREQUAL "multi")
    set(_gliner_repo "onnx-community/gliner_multi-v2.1")
    set(_gliner_revision "6ddaeb9413b0e71ad8457da1aab378a165b24058")
    set(_gliner_files
        "onnx/model_int8.onnx|model.onnx|995058c82c5f570601dd8a0ba74ee60a392f764268bc5f628455e44dd3b476ec"
        "spm.model|spm.model|13c8d666d62a7bc4ac8f040aab68e942c861f93303156cc28f5c7e885d86d6e3"
        "added_tokens.json|added_tokens.json|030e747c4ca7992a3ac794c6fda9919352c88ae722e85178217cd083b450078d"
        "gliner_config.json|gliner_config.json|1ef59c57fe6816a155697a5670ca8d0faf0babe13f199a2c2b5b65113399ed72")
else()
    set(_gliner_repo "onnx-community/gliner_small-v2.1")
    set(_gliner_revision "8142fb00740ccea973e64b1272949ff48653df5e")
    set(_gliner_files
        "onnx/model_int8.onnx|model.onnx|c76c90920547fd937aaf505e7f2de5ec73168bf1c25abbb55a298104cb061400"
        "spm.model|spm.model|c679fbf93643d19aab7ee10c0b99e460bdbc02fedf34b92b05af343b4af586fd"
        "added_tokens.json|added_tokens.json|0a4df7f7953a90443fa51b6ca723242f6cb29e7fc34b1f27fc320c8e60de45b7"
        "gliner_config.json|gliner_config.json|8e8b59de124a256a3f3de67879d0da686fe3f73ecc05093506ba525e451b920d")
endif()

# The variant downloaded last; another one replaces it.
set(_gliner_stamp "${GLINER_MODEL_DIR}/variant.txt")
if(EXISTS "${_gliner_stamp}")
    file(READ "${_gliner_stamp}" _gliner_had)
    if(NOT _gliner_had STREQUAL "${_gliner_revision}")
        file(REMOVE_RECURSE "${GLINER_MODEL_DIR}")
    endif()
endif()
file(MAKE_DIRECTORY "${GLINER_MODEL_DIR}")

set(_gliner_ok ON)
foreach(_entry ${_gliner_files})
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _remote)
    list(GET _parts 1 _local)
    list(GET _parts 2 _hash)
    set(_dest "${GLINER_MODEL_DIR}/${_local}")
    if(NOT EXISTS "${_dest}")
        message(STATUS "Downloading GLiNER ${_local} (${_gliner_repo})...")
        file(DOWNLOAD "https://huggingface.co/${_gliner_repo}/resolve/${_gliner_revision}/${_remote}" "${_dest}"
             EXPECTED_HASH SHA256=${_hash} STATUS _status TIMEOUT 900)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_dest}")
            set(_gliner_ok OFF)
            message(WARNING "GLiNER ${_local} not downloaded: ${_status}; entity_model gliner needs it at run time")
        endif()
    endif()
endforeach()
if(_gliner_ok)
    file(WRITE "${_gliner_stamp}" "${_gliner_revision}")
endif()
