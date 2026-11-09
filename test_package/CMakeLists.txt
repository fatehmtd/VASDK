# The programs of tests/consumer/c_library (C, and C++ through vasdk.hpp), built against the
# Conan package with find_package(VASDK)
cmake_minimum_required(VERSION 3.16)
project(test_package LANGUAGES C CXX)

find_package(VASDK REQUIRED COMPONENTS c)

set(VASDK_CONSUMER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../tests/consumer/c_library)

add_executable(test_package ${VASDK_CONSUMER_SOURCES}/main.c)
target_link_libraries(test_package PRIVATE VASDK::c)
vasdk_copy_runtime(test_package)   # Windows: the package's DLLs beside it, ahead of System32's onnxruntime.dll

add_executable(test_package_cpp ${VASDK_CONSUMER_SOURCES}/cpp_main.cpp)
target_compile_features(test_package_cpp PRIVATE cxx_std_17)
target_link_libraries(test_package_cpp PRIVATE VASDK::c)
vasdk_copy_runtime(test_package_cpp)
