cmake_minimum_required(VERSION 3.16)
project(MemoryPool)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(memory_pool_core
            src/MetaData.cc
            src/CentralCache.cc
            src/PageCache.cc
            src/ThreadCache.cc)

target_include_directories(memory_pool_core PUBLIC inc)
target_compile_options(memory_pool_core PRIVATE -Wall -Wextra -Werror)

add_executable(memory_pool src/main.cc)
target_link_libraries(memory_pool PRIVATE memory_pool_core)

enable_testing()
add_subdirectory(test)
add_subdirectory(benchmark)