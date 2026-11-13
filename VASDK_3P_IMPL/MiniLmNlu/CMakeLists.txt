# all-MiniLM-L6-v2 sentence encoder: an agent understands paraphrases its examples miss.
add_library(vasdk_minilm_nlu ${VASDK_THIRDPARTY_LIB_TYPE} MiniLmSentenceEncoder.h MiniLmSentenceEncoder.cpp)
target_include_directories(vasdk_minilm_nlu PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(vasdk_minilm_nlu PUBLIC VASDK onnxruntime_lib spdlog::spdlog)
target_compile_features(vasdk_minilm_nlu PUBLIC cxx_std_17)

if(VASDK_BUILD_TESTS AND NOT ANDROID)
    vasdk_add_gtest(vasdk_minilm_nlu LABEL model   # skipped without the model
        SOURCES minilm_nlu_test.cpp LIBRARIES vasdk_minilm_nlu MODELS_DIR ${MINILM_MODEL_DIR})
endif()
