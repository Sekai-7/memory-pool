include(FetchContent)
FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(memory_pool_benchmark memory_pool_benchmark.cc)
target_link_libraries(memory_pool_benchmark PRIVATE memory_pool_core benchmark::benchmark benchmark::benchmark_main pthread)
