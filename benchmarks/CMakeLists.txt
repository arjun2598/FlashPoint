# Google Benchmark is fetched at configure time and pinned to an exact release
# tag, the same way GoogleTest is (DD-005).
#
# SYSTEM keeps its headers out of our -Werror policy.
include(FetchContent)

FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.5
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

# ---------------------------------------------------------------------------
# Two executables, because they answer two different questions.
#
# Throughput is "operations per second under saturation", which Google Benchmark
# handles well. Latency is the distribution per operation, and Google Benchmark
# reports mean and median rather than the tail, so that one is measured by hand
# (DD-039).
# ---------------------------------------------------------------------------

add_executable(flashpoint_throughput_bench throughput_bench.cpp)
target_link_libraries(flashpoint_throughput_bench
    PRIVATE
        FlashPoint::flashpoint
        flashpoint_warnings
        benchmark::benchmark)
target_include_directories(flashpoint_throughput_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(flashpoint_throughput_bench PROPERTIES CXX_EXTENSIONS OFF)

add_executable(flashpoint_latency_bench latency_bench.cpp)
target_link_libraries(flashpoint_latency_bench
    PRIVATE
        FlashPoint::flashpoint
        flashpoint_warnings)
target_include_directories(flashpoint_latency_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(flashpoint_latency_bench PROPERTIES CXX_EXTENSIONS OFF)
