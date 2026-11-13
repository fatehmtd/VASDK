# Registering tests, the one way every test is registered (skills/vasdk-testing, docs/testing.md).
#
#   vasdk_add_gtest(<name> LABEL <label> SOURCES <files>... [LIBRARIES <targets>...]
#                   [MODELS_DIR <folder>] [ARGS <arguments>...] [TIMEOUT <seconds>] [RUN_SERIAL]
#                   [WORKING_DIRECTORY <folder>])
#       A GoogleTest program, <name>_test, linked to the shared test support (VASDK/tests/support:
#       its main, waits, temporary folders, models). Each test case is its own ctest test,
#       <name>.<Suite>.<Case>, so one case can be run, filtered, retried or skipped. GTEST_SKIP()
#       shows as skipped. MODELS_DIR reaches the program as --vasdk-models-dir, read by
#       va::test::modelsDirectory(). Cross-compiled (Android), the program is one ctest test with
#       the same arguments, which VASDK_ANDROID/run-tests-on-device.sh runs on a device.
#
#   vasdk_add_pytest(<name> LABEL <label> FILE <test_file.py> [ARGS <arguments>...]
#                    [ENVIRONMENT <VAR=value>...] [TIMEOUT <seconds>])
#       A pytest file, one ctest test, run by the Python3_EXECUTABLE found before; pytest.skip()
#       shows as skipped.
#
#   vasdk_label_tests(<label> <tests>...)
#       The label of tests registered another way: agent suites, eval runs, scripts.
#
#   vasdk_test_dlls(<target>)
#       A program's DLLs beside it on Windows (no rpath there): a shared VASDK and ONNX Runtime.
#
# Every test has exactly one label, its layer; vasdk_test_labels fails on a test without one.

include_guard(GLOBAL)

# The layers: what a test needs and how fast it runs (docs/testing.md#layers-and-labels)
set(VASDK_TEST_LABELS unit timing integration model agent eval binding packaging)

# GoogleTest and gMock (the tests' framework, never shipped) and vasdk_test_support (VASDK/tests/support:
# the tests' main, waits, temporary folders, models). A macro: vasdk_dependency's variables reach
# the caller. Called by the root CMakeLists.txt and VASDK_ANDROID's, with VASDK_BUILD_TESTS on.
macro(vasdk_add_test_support)
    set(INSTALL_GTEST OFF CACHE BOOL "googletest: install its files (VASDK: no)")
    set(gtest_force_shared_crt ON CACHE BOOL "googletest: the DLL C runtime on MSVC, as VASDK")
    vasdk_dependency(GTest TARGET GTest::gmock
        FIND CONFIG
        DECLARE GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG        b514bdc898e2951020cbdca1304b75f5950d1f59)   # v1.15.2
    add_subdirectory(${VASDK_ROOT_DIR}/VASDK/tests/support ${VASDK_ROOT_BINARY_DIR}/vasdk_test_support)
endmacro()

function(_vasdk_check_test_label label)
    if(NOT label IN_LIST VASDK_TEST_LABELS)
        message(FATAL_ERROR "VASDK: test label '${label}' is not one of: ${VASDK_TEST_LABELS} (docs/testing.md)")
    endif()
endfunction()

function(vasdk_label_tests label)
    _vasdk_check_test_label(${label})
    set_tests_properties(${ARGN} PROPERTIES LABELS ${label})
endfunction()

function(vasdk_test_dlls target)
    if(NOT WIN32)
        return()
    endif()
    get_target_property(_vasdk_type VASDK TYPE)
    if(_vasdk_type STREQUAL "SHARED_LIBRARY")   # VASDK.dll and what it loads (protobuf, curl, OpenSSL, zlib)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:VASDK> $<TARGET_RUNTIME_DLLS:VASDK>
                    $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
    if(DEFINED onnxruntime_SOURCE_DIR AND EXISTS "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
                    $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

function(vasdk_add_pytest name)
    cmake_parse_arguments(PARSE_ARGV 1 _vasdk_test "" "LABEL;FILE;TIMEOUT" "ARGS;ENVIRONMENT")
    if(NOT _vasdk_test_LABEL OR NOT _vasdk_test_FILE)
        message(FATAL_ERROR "vasdk_add_pytest(${name}): LABEL and FILE are required")
    endif()
    _vasdk_check_test_label(${_vasdk_test_LABEL})
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "vasdk_add_pytest(${name}): find_package(Python3 COMPONENTS Interpreter) first")
    endif()
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE} -m pytest ${_vasdk_test_FILE} -q -rs ${_vasdk_test_ARGS})
    # pytest exits 0 when everything it ran was skipped; its summary is then "<n> skipped in <time>"
    # alone (with a pass or a failure it starts with those)
    set_tests_properties(${name} PROPERTIES LABELS ${_vasdk_test_LABEL} SKIP_REGULAR_EXPRESSION "\n[0-9]+ skipped in ")
    if(_vasdk_test_ENVIRONMENT)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_vasdk_test_ENVIRONMENT}")
    endif()
    if(_vasdk_test_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${_vasdk_test_TIMEOUT})
    endif()
endfunction()

function(vasdk_add_gtest name)
    cmake_parse_arguments(PARSE_ARGV 1 _vasdk_test "RUN_SERIAL" "LABEL;MODELS_DIR;TIMEOUT;WORKING_DIRECTORY"
                          "SOURCES;LIBRARIES;ARGS")
    if(NOT _vasdk_test_LABEL)
        message(FATAL_ERROR "vasdk_add_gtest(${name}): LABEL is required, one of: ${VASDK_TEST_LABELS}")
    endif()
    _vasdk_check_test_label(${_vasdk_test_LABEL})
    if(NOT _vasdk_test_SOURCES)
        message(FATAL_ERROR "vasdk_add_gtest(${name}): SOURCES is required")
    endif()

    set(_target ${name}_test)
    add_executable(${_target} ${_vasdk_test_SOURCES})
    target_link_libraries(${_target} PRIVATE vasdk_test_support ${_vasdk_test_LIBRARIES})
    vasdk_test_dlls(${_target})

    set(_arguments ${_vasdk_test_ARGS})
    if(_vasdk_test_MODELS_DIR)
        list(APPEND _arguments "--vasdk-models-dir=${_vasdk_test_MODELS_DIR}")
    endif()
    # GTEST_SKIP() prints "[  SKIPPED ]"; ctest reports the case as skipped, not passed
    set(_properties LABELS ${_vasdk_test_LABEL} SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
    if(_vasdk_test_TIMEOUT)
        list(APPEND _properties TIMEOUT ${_vasdk_test_TIMEOUT})
    endif()
    if(_vasdk_test_RUN_SERIAL)
        list(APPEND _properties RUN_SERIAL TRUE)
    endif()
    set(_working_directory ${CMAKE_CURRENT_BINARY_DIR})
    if(_vasdk_test_WORKING_DIRECTORY)
        set(_working_directory ${_vasdk_test_WORKING_DIRECTORY})
    endif()

    if(CMAKE_CROSSCOMPILING)
        # The cases can't be listed by running the program here: one test, the whole program
        add_test(NAME ${name} COMMAND ${_target} ${_arguments} WORKING_DIRECTORY ${_working_directory})
        set_tests_properties(${name} PROPERTIES ${_properties})
    else()
        include(GoogleTest)
        # PRE_TEST: the cases are listed when ctest runs, not at build time. NO_PRETTY_VALUES: a
        # parameterized case is named by its name generator (.../VadSilence), not its value printed
        gtest_discover_tests(${_target}
            TEST_PREFIX "${name}."
            EXTRA_ARGS ${_arguments}
            WORKING_DIRECTORY ${_working_directory}
            NO_PRETTY_VALUES
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 60
            PROPERTIES ${_properties})
    endif()
endfunction()
