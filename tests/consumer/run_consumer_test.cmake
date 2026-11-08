# Builds and runs tests/consumer/c_library against this build of VASDK, as another project would:
#   MODE=install     cmake --install into a fresh prefix, then find_package(VASDK) from it
#   MODE=build_tree  find_package(VASDK) straight from the build tree (VASDK_DIR)
# cmake -DMODE=... -DVASDK_BUILD_DIR=... -DCONFIG=... -DGENERATOR=... [-DPLATFORM=...] -DWORK_DIR=... -P run_consumer_test.cmake

foreach(_required MODE VASDK_BUILD_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "run_consumer_test.cmake: ${_required} is not set")
    endif()
endforeach()
if(NOT CONFIG)
    set(CONFIG Release)
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
    set(_find -DCMAKE_PREFIX_PATH=${_prefix})
elseif(MODE STREQUAL "build_tree")
    set(_find -DVASDK_DIR=${VASDK_BUILD_DIR})
else()
    message(FATAL_ERROR "MODE is '${MODE}': install or build_tree")
endif()

get_filename_component(_source ${CMAKE_CURRENT_LIST_DIR}/c_library ABSOLUTE)
run(${CMAKE_COMMAND} -S ${_source} -B ${WORK_DIR}/consumer -G ${GENERATOR} ${_platform} ${_find} -DCMAKE_BUILD_TYPE=${CONFIG})
run(${CMAKE_COMMAND} --build ${WORK_DIR}/consumer --config ${CONFIG})

# Windows: vasdk_copy_runtime() put vasdk_c.dll and what it loads beside the program, so no PATH
# is needed; elsewhere the rpath finds them
foreach(_program vasdk_consumer vasdk_consumer_cpp)   # C, and C++ through vasdk.hpp
    find_program(_consumer_${_program} ${_program} PATHS ${WORK_DIR}/consumer/${CONFIG} ${WORK_DIR}/consumer NO_DEFAULT_PATH)
    if(NOT _consumer_${_program})
        message(FATAL_ERROR "${_program} was not built")
    endif()
    run(${_consumer_${_program}})
endforeach()
