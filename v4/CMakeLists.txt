cmake_minimum_required(VERSION 3.16)
project(MemoryPool)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS "Enable unit-test-only hooks in memory_pool_core" OFF)

include(CTest)

add_library(memory_pool_core
            src/MetaData.cc
            src/CentralCache.cc
            src/PageCache.cc
            src/ThreadCache.cc)

target_include_directories(memory_pool_core PUBLIC inc)
target_compile_options(memory_pool_core PRIVATE -Wall -Wextra -Werror)
if(MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS)
    target_compile_definitions(memory_pool_core PRIVATE MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS)
endif()

add_executable(memory_pool src/main.cc)
target_link_libraries(memory_pool PRIVATE memory_pool_core)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
add_subdirectory(benchmark)
