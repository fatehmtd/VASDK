# Another project that adds VASDK from source, as docs/integrating.md shows: FetchContent, here
# from a local checkout (-DFETCHCONTENT_SOURCE_DIR_VASDK=<repository>). CI builds it; it checks that
# VASDK stays out of the way as a subproject and that both forms link.
cmake_minimum_required(VERSION 3.21)
project(vasdk_subproject LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(VASDK GIT_REPOSITORY https://github.com/fatehmtd/VASDK.git GIT_TAG master)
set(VASDK_FLAVOR default)
FetchContent_MakeAvailable(VASDK)

# As a subproject: no sample app, no test, no model download
foreach(_unexpected VASDK_Pipeline VASDK_CallCenter VASDK_Car vasdk_session_test)
    if(TARGET ${_unexpected})
        message(FATAL_ERROR "VASDK as a subproject built ${_unexpected}")
    endif()
endforeach()
if(VASDK_DOWNLOAD_MODELS)
    message(FATAL_ERROR "VASDK as a subproject downloads models")
endif()
get_property(_shared_cached CACHE BUILD_SHARED_LIBS PROPERTY VALUE)
if(_shared_cached)
    message(FATAL_ERROR "VASDK set BUILD_SHARED_LIBS in the cache")
endif()

# The C++ API
add_executable(cpp_consumer cpp_main.cpp)
target_link_libraries(cpp_consumer PRIVATE VASDK::VASDK VASDK::providers)

# The C library
add_executable(c_consumer ${CMAKE_CURRENT_SOURCE_DIR}/../c_library/main.c)
target_link_libraries(c_consumer PRIVATE VASDK::c)
vasdk_copy_runtime(c_consumer)   # Windows: vasdk_c.dll and what it loads beside it (no rpath there)

enable_testing()
add_test(NAME cpp_consumer COMMAND cpp_consumer)
add_test(NAME c_consumer COMMAND c_consumer)
