# Every test has a layer label (cmake/VASDKTesting.cmake, docs/testing.md): lists this build's tests
# that carry none of them and fails when there is one.
#   cmake -DCTEST=<ctest> -DBUILD_DIR=<build> -DCONFIG=<config> -DLABELS="unit|timing|..." -P check_test_labels.cmake

execute_process(COMMAND ${CTEST} --test-dir ${BUILD_DIR} -C ${CONFIG} -N -LE "^(${LABELS})$"
                OUTPUT_VARIABLE _vasdk_listed ERROR_VARIABLE _vasdk_errors RESULT_VARIABLE _vasdk_result)
if(NOT _vasdk_result EQUAL 0)
    message(FATAL_ERROR "ctest -N failed (${_vasdk_result}):\n${_vasdk_errors}")
endif()
string(REGEX MATCHALL "Test +#[0-9]+: [^\n]+" _vasdk_unlabelled "${_vasdk_listed}")
if(_vasdk_unlabelled)
    string(REPLACE ";" "\n  " _vasdk_unlabelled "${_vasdk_unlabelled}")
    message(FATAL_ERROR "tests without a layer label (${LABELS}): register them with vasdk_add_gtest or "
                        "label them with vasdk_label_tests (cmake/VASDKTesting.cmake):\n  ${_vasdk_unlabelled}")
endif()
message(STATUS "every test has a layer label")
