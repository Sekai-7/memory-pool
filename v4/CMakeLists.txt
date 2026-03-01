cmake_minimum_required(3.28)
project(MemoryPool)

add_library(memory_pool_lib STATIC
            src/CentralCache.cc
            src/PageCache.cc
            src/ThreadCache.cc)

target_include_directories(memory_pool_lib PUBLIC inc)

target_compile_features(memory_pool_lib PUBLIC cxx_std_23)

target_compile_options(memory_pool_lib PRIVATE -Wall -Wextra -Werror)