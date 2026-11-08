# A program that uses VASDK the way another project does: find_package(VASDK) and VASDK::c, from
# an install prefix or a build tree (tests/consumer/run_consumer_test.cmake drives it).
cmake_minimum_required(VERSION 3.16)
project(vasdk_consumer LANGUAGES C)

find_package(VASDK 1.0 REQUIRED COMPONENTS c)

add_executable(vasdk_consumer main.c)
target_link_libraries(vasdk_consumer PRIVATE VASDK::c)
vasdk_copy_runtime(vasdk_consumer)   # Windows: vasdk_c.dll and what it loads beside it
