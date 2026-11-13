# What every test links (vasdk_add_gtest does it): GoogleTest and gMock, the tests' main, and
# TestSupport.h (va::test: waitUntil, UniqueTemporaryDirectory, modelsDirectory, VA_REQUIRE_FILE).
add_library(vasdk_test_support STATIC TestMain.cpp TestSupport.h)
target_include_directories(vasdk_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vasdk_test_support PUBLIC GTest::gmock)
target_compile_features(vasdk_test_support PUBLIC cxx_std_17)
