# all-MiniLM-L6-v2 sentence encoder: an agent understands paraphrases its examples miss.
add_library(vasdk_minilm_nlu ${VASDK_THIRDPARTY_LIB_TYPE} MiniLmSentenceEncoder.h MiniLmSentenceEncoder.cpp)
target_include_directories(vasdk_minilm_nlu PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_link_libraries(vasdk_minilm_nlu PUBLIC VASDK onnxruntime_lib spdlog::spdlog)
target_compile_features(vasdk_minilm_nlu PUBLIC cxx_std_17)

if(VASDK_BUILD_TESTS AND NOT ANDROID)
    add_executable(vasdk_minilm_nlu_test minilm_nlu_test.cpp)
    target_link_libraries(vasdk_minilm_nlu_test PRIVATE vasdk_minilm_nlu)
    add_test(NAME vasdk_minilm_nlu COMMAND vasdk_minilm_nlu_test ${MINILM_MODEL_DIR})
    set_tests_properties(vasdk_minilm_nlu PROPERTIES SKIP_RETURN_CODE 77)   # without the model
endif()
