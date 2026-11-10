# A program that uses VASDK's C++ libraries the way another project does: find_package(VASDK
# COMPONENTS cpp), from an install prefix or a build tree (tests/consumer/run_consumer_test.cmake
# drives it with PACKAGE=cpp). Its dependencies (protobuf, spdlog...) come from where VASDK's did:
# the same CMAKE_PREFIX_PATH or toolchain file.
cmake_minimum_required(VERSION 3.21)
project(vasdk_cpp_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(VASDK 1.0 REQUIRED COMPONENTS cpp)
message(STATUS "VASDK ${VASDK_VERSION}: C++ providers '${VASDK_CPP_PROVIDERS}', catalog ${VASDK_CPP_HAS_CATALOG}")

add_executable(vasdk_cpp_consumer main.cpp)
target_link_libraries(vasdk_cpp_consumer PRIVATE VASDK::VASDK)
if(VASDK_CPP_HAS_CATALOG)
    target_link_libraries(vasdk_cpp_consumer PRIVATE VASDK::providers)
    target_compile_definitions(vasdk_cpp_consumer PRIVATE VASDK_CONSUMER_HAS_CATALOG)
endif()
