cmake_minimum_required(VERSION 4.0)

project(Phanes
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


include(FetchContent)

FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        dev
    SOURCE_SUBDIR  build/cmake
)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "Don't build the xxhsum CLI" FORCE)
FetchContent_MakeAvailable(xxhash)

enable_testing()

# AVX2 is required by the SIMD hash in analyzer/hasher.cpp
add_compile_options(
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-mavx2>
    $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX2>
)

add_subdirectory(src)

# todo tests should be optional?
add_subdirectory(tests)

option(PHANES_BUILD_BENCHMARKS "Build Google Benchmark benchmarks" OFF)
if(PHANES_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
