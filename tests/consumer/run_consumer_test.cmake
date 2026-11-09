# Builds and runs a program against this build of VASDK, as another project would:
#   MODE=install     cmake --install into a fresh prefix, then find_package(VASDK) from it
#   MODE=build_tree  find_package(VASDK) straight from the build tree (VASDK_DIR)
#   PACKAGE=c        tests/consumer/c_library: the C library, VASDK::c (the default)
#   PACKAGE=cpp      tests/consumer/cpp_library: the C++ libraries, find_package(VASDK COMPONENTS cpp).
#                    The install adds the component vasdk_cpp_development. The program finds VASDK's
#                    dependencies as VASDK's build did: DEPENDENCY_TOOLCHAIN (its toolchain file: Conan,
#                    vcpkg) and DEPENDENCY_PREFIX_PATH (its CMAKE_PREFIX_PATH, entries joined by |).
# cmake -DMODE=... [-DPACKAGE=...] -DVASDK_BUILD_DIR=... -DCONFIG=... -DGENERATOR=... [-DPLATFORM=...]
#       [-DDEPENDENCY_TOOLCHAIN=...] [-DDEPENDENCY_PREFIX_PATH=...] -DWORK_DIR=... -P run_consumer_test.cmake

foreach(_required MODE VASDK_BUILD_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "run_consumer_test.cmake: ${_required} is not set")
    endif()
endforeach()
if(NOT CONFIG)
    set(CONFIG Release)
endif()
if(NOT PACKAGE)
    set(PACKAGE c)
endif()
if(NOT PACKAGE MATCHES "^(c|cpp)$")
    message(FATAL_ERROR "PACKAGE is '${PACKAGE}': c or cpp")
endif()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Failed (${_result}): ${ARGN}\n${_output}")
    endif()
    message(STATUS "${_output}")
endfunction()

file(REMOVE_RECURSE ${WORK_DIR})
set(_platform)
if(PLATFORM)
    set(_platform -A ${PLATFORM})
endif()

if(MODE STREQUAL "install")
    set(_prefix ${WORK_DIR}/prefix)
    run(${CMAKE_COMMAND} --install ${VASDK_BUILD_DIR} --prefix ${_prefix} --config ${CONFIG})
    # A plain install has no sample app and no C++ library
    file(GLOB_RECURSE _unexpected ${_prefix}/*VASDK_Pipeline* ${_prefix}/*VASDK_CallCenter* ${_prefix}/*VASDK_Car*)
    if(_unexpected)
        message(FATAL_ERROR "A plain install holds a sample app: ${_unexpected}")
    endif()
    # ... nor a dependency's own files, nor the static C++ libraries: VASDK's headers and package only
    file(GLOB _includes RELATIVE ${_prefix}/include ${_prefix}/include/*)
    file(GLOB _packages RELATIVE ${_prefix}/lib/cmake ${_prefix}/lib/cmake/*)
    file(GLOB _static_libraries ${_prefix}/lib/*.a ${_prefix}/lib/VASDK.lib ${_prefix}/lib/vasdk_*.lib)
    list(REMOVE_ITEM _static_libraries ${_prefix}/lib/vasdk_c.lib)   # the DLL's import library
    if(NOT _includes STREQUAL "VASDK" OR NOT _packages STREQUAL "VASDK" OR _static_libraries)
        message(FATAL_ERROR "A plain install holds more than VASDK's C library: include/ has '${_includes}', "
                            "lib/cmake/ has '${_packages}', static libraries: '${_static_libraries}'")
    endif()
    # The installed command runs from the prefix: it finds the library (rpath, or the DLLs beside it)
    find_program(_vasdk_command vasdk PATHS ${_prefix}/bin NO_DEFAULT_PATH)
    if(NOT _vasdk_command)
        message(FATAL_ERROR "A plain install has no bin/vasdk")
    endif()
    run(${_vasdk_command} version)
    if(PACKAGE STREQUAL "cpp")
        # The C++ libraries on top of the plain install, and their package
        run(${CMAKE_COMMAND} --install ${VASDK_BUILD_DIR} --prefix ${_prefix} --config ${CONFIG}
            --component vasdk_cpp_development)
        foreach(_file lib/cmake/VASDK/VASDKCpp.cmake lib/cmake/VASDK/VASDKCppTargets.cmake
                      include/VASDK/engine/ProviderRegistry.h include/VASDK/proto/vasdk/engine/engine_config.pb.h)
            if(NOT EXISTS ${_prefix}/${_file})
                message(FATAL_ERROR "The component vasdk_cpp_development installs no ${_file}")
            endif()
        endforeach()
    endif()
    set(_find -DCMAKE_PREFIX_PATH=${_prefix})
elseif(MODE STREQUAL "build_tree")
    set(_find -DVASDK_DIR=${VASDK_BUILD_DIR})
else()
    message(FATAL_ERROR "MODE is '${MODE}': install or build_tree")
endif()

if(PACKAGE STREQUAL "cpp")
    # VASDK by its package folder, so CMAKE_PREFIX_PATH stays free for VASDK's own dependencies,
    # found where VASDK's build found them: its toolchain file and prefix path, through an initial
    # cache, which keeps a list a list
    if(MODE STREQUAL "install")
        set(_find -DVASDK_DIR=${_prefix}/lib/cmake/VASDK)
    else()
        set(_find -DVASDK_DIR=${VASDK_BUILD_DIR})
    endif()
    set(_initial_cache ${WORK_DIR}/dependencies.cmake)
    file(WRITE ${_initial_cache} "")
    if(DEPENDENCY_TOOLCHAIN)
        file(APPEND ${_initial_cache} "set(CMAKE_TOOLCHAIN_FILE [==[${DEPENDENCY_TOOLCHAIN}]==] CACHE FILEPATH \"\")\n")
    endif()
    if(DEPENDENCY_PREFIX_PATH)
        string(REPLACE "|" ";" _prefix_path "${DEPENDENCY_PREFIX_PATH}")
        file(APPEND ${_initial_cache} "set(CMAKE_PREFIX_PATH [==[${_prefix_path}]==] CACHE STRING \"\")\n")
    endif()
    list(APPEND _find -C ${_initial_cache})
    set(_programs vasdk_cpp_consumer)
    get_filename_component(_source ${CMAKE_CURRENT_LIST_DIR}/cpp_library ABSOLUTE)
else()
    set(_programs vasdk_consumer vasdk_consumer_cpp)   # C, and C++ through vasdk.hpp
    get_filename_component(_source ${CMAKE_CURRENT_LIST_DIR}/c_library ABSOLUTE)
endif()
run(${CMAKE_COMMAND} -S ${_source} -B ${WORK_DIR}/consumer -G ${GENERATOR} ${_platform} ${_find} -DCMAKE_BUILD_TYPE=${CONFIG})
run(${CMAKE_COMMAND} --build ${WORK_DIR}/consumer --config ${CONFIG})

# Windows: vasdk_copy_runtime() put vasdk_c.dll and what it loads beside the program, so no PATH
# is needed; elsewhere the rpath finds them
foreach(_program IN LISTS _programs)
    find_program(_consumer_${_program} ${_program} PATHS ${WORK_DIR}/consumer/${CONFIG} ${WORK_DIR}/consumer NO_DEFAULT_PATH)
    if(NOT _consumer_${_program})
        message(FATAL_ERROR "${_program} was not built")
    endif()
    run(${_consumer_${_program}})
endforeach()
