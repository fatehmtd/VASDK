# GLiNER zero-shot entity model: where an agent's entities are said (engine config entity_model { id: "gliner" }).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/FetchSentencepiece.cmake)

add_library(vasdk_gliner_ner ${VASDK_THIRDPARTY_LIB_TYPE} GlinerEntityExtractor.h GlinerEntityExtractor.cpp)
target_include_directories(vasdk_gliner_ner
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    PRIVATE ${sentencepiece_SOURCE_DIR}/src)
target_link_libraries(vasdk_gliner_ner
    PUBLIC VASDK onnxruntime_lib spdlog::spdlog
    PRIVATE nlohmann_json::nlohmann_json sentencepiece-static)
target_compile_features(vasdk_gliner_ner PUBLIC cxx_std_17)

if(VASDK_BUILD_TESTS AND NOT ANDROID)
    add_executable(vasdk_gliner_ner_test gliner_ner_test.cpp)
    target_link_libraries(vasdk_gliner_ner_test PRIVATE vasdk_gliner_ner)
    vasdk_test_dlls(vasdk_gliner_ner_test)
    add_test(NAME vasdk_gliner_ner COMMAND vasdk_gliner_ner_test ${GLINER_MODEL_DIR})
    vasdk_label_tests(model vasdk_gliner_ner)
    set_tests_properties(vasdk_gliner_ner PROPERTIES SKIP_RETURN_CODE 77)   # without the model
endif()
