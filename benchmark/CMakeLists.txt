include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

add_executable(phanes_bench
    bench_main.cpp
)

target_link_libraries(phanes_bench
    PRIVATE
        phanes_core
        benchmark::benchmark_main
)

target_compile_options(phanes_bench PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>
    $<$<CXX_COMPILER_ID:MSVC>:/O2>
)
