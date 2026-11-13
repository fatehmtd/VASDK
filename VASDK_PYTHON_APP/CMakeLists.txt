# The kitchen assistant is Python: nothing to build. Its tests run in ctest when there's a Python 3
# and the C library (vasdk_c, what the binding loads):
#   vasdk_python_kitchen   the kitchen's code, and every conversation in tests/*.txtpb
#   vasdk_python_session   a spoken session from Python, with the speech models (skipped without them)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT TARGET vasdk_c)
    return()
endif()

add_test(NAME vasdk_python_kitchen COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_kitchen.py)
vasdk_label_tests(binding vasdk_python_kitchen)
set_tests_properties(vasdk_python_kitchen PROPERTIES ENVIRONMENT "${VASDK_PYTHON_TEST_ENVIRONMENT}")

if(TARGET vasdk_provider_catalog)
    add_test(NAME vasdk_python_session
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_session.py ${VASDK_ROOT_BINARY_DIR}/VASDK_3P_IMPL/models)
    vasdk_label_tests(binding vasdk_python_session)
    set_tests_properties(vasdk_python_session PROPERTIES
        ENVIRONMENT "${VASDK_PYTHON_TEST_ENVIRONMENT}"
        SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
