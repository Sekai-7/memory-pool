cmake_minimum_required(VERSION 3.16)
project(MemoryPool)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MEMORY_POOL_BUILD_MODE "PERF" CACHE STRING "Build mode for MemoryPool (PERF or BENCHMARK)")
set_property(CACHE MEMORY_POOL_BUILD_MODE PROPERTY STRINGS PERF BENCHMARK)

if(NOT MEMORY_POOL_BUILD_MODE STREQUAL "PERF" AND NOT MEMORY_POOL_BUILD_MODE STREQUAL "BENCHMARK")
    message(FATAL_ERROR "MEMORY_POOL_BUILD_MODE must be PERF or BENCHMARK")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES)
    if(MEMORY_POOL_BUILD_MODE STREQUAL "PERF")
        if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "" AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
            message(STATUS "MEMORY_POOL_BUILD_MODE=PERF overrides CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -> RelWithDebInfo")
        endif()
        set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
        set(BUILD_TESTING ON CACHE BOOL "Enable testing" FORCE)
        set(MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS ON CACHE BOOL "Enable unit-test-only hooks in memory_pool_core" FORCE)
    else()
        if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "" AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
            message(STATUS "MEMORY_POOL_BUILD_MODE=BENCHMARK overrides CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -> Release")
        endif()
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "Enable testing" FORCE)
        set(MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS OFF CACHE BOOL "Enable unit-test-only hooks in memory_pool_core" FORCE)
    endif()
endif()

message(STATUS "MemoryPool build mode: ${MEMORY_POOL_BUILD_MODE}")
message(STATUS "MemoryPool build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "MemoryPool BUILD_TESTING: ${BUILD_TESTING}")
message(STATUS "MemoryPool unit test hooks: ${MEMORY_POOL_ENABLE_UNIT_TEST_HOOKS}")
if(MEMORY_POOL_BUILD_MODE STREQUAL "PERF")
    message(STATUS "MemoryPool frame pointers: ON (-fno-omit-frame-pointer)")
else()
    message(STATUS "MemoryPool frame pointers: OFF")
endif()

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
if(MEMORY_POOL_BUILD_MODE STREQUAL "PERF")
    target_compile_options(memory_pool_core PRIVATE -fno-omit-frame-pointer)
endif()

add_executable(memory_pool src/main.cc)
target_link_libraries(memory_pool PRIVATE memory_pool_core)
if(MEMORY_POOL_BUILD_MODE STREQUAL "PERF")
    target_compile_options(memory_pool PRIVATE -fno-omit-frame-pointer)
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
add_subdirectory(benchmark)
