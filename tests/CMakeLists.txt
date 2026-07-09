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

file(GLOB PHANES_TEST_SOURCES "*.cpp")

if(WIN32)
    list(APPEND PHANES_TEST_SOURCES io_test.cpp)
endif()

add_executable(phanes_test
    ${PHANES_TEST_SOURCES}
)

target_link_libraries(phanes_test
    PRIVATE
        phanes_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(phanes_test)