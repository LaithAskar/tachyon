include(FetchContent)

set(BENCHMARK_ENABLE_TESTING       OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS   OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL       OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz
)
FetchContent_MakeAvailable(benchmark)

# Throughput benchmarks via Google Benchmark.
add_executable(bench_throughput bench_throughput.cpp)
target_link_libraries(bench_throughput
    PRIVATE
        tachyon_core
        benchmark::benchmark
        benchmark::benchmark_main
)

# Latency percentile harness, no GB dependency (manual histogram).
add_executable(bench_latency bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE tachyon_core)
