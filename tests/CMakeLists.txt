cmake_minimum_required(VERSION 4.0)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
# Prevent GTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(parser_test parser_test.cpp)
target_link_libraries(parser_test PRIVATE phanes_core GTest::gtest_main)

add_executable(analyzer_test analyzer_test.cpp)
target_link_libraries(analyzer_test PRIVATE phanes_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(parser_test)
gtest_discover_tests(analyzer_test)
