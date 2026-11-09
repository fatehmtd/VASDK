# --- GLiNER entity model, int8 ONNX: manifest id gliner (small) or gliner_multi ---
# VASDK_GLINER_MODEL: small (gliner_small-v2.1, English, 183 MB) or multi (gliner_multi-v2.1,
# multilingual, 349 MB). Both Apache-2.0, ONNX exports by onnx-community. Both land in the same
# files, so switching replaces them.
set(VASDK_GLINER_MODEL "small" CACHE STRING "GLiNER model: small (English) or multi (multilingual)")
set_property(CACHE VASDK_GLINER_MODEL PROPERTY STRINGS small multi)
set(GLINER_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models/gliner")
if(VASDK_GLINER_MODEL STREQUAL "multi")
    vasdk_download_model(gliner_multi DIRECTORY "${GLINER_MODEL_DIR}" STRIP gliner/)
elseif(VASDK_GLINER_MODEL STREQUAL "small")
    vasdk_download_model(gliner DIRECTORY "${GLINER_MODEL_DIR}" STRIP gliner/)
else()
    message(FATAL_ERROR "VASDK_GLINER_MODEL must be small or multi (got '${VASDK_GLINER_MODEL}')")
endif()
