cmake_minimum_required(VERSION 3.28)
project(MemoryPool)

add_executable(memory_pool
            src/main.cc
            src/MetaData.cc
            src/CentralCache.cc
            src/PageCache.cc
            src/ThreadCache.cc)

target_include_directories(memory_pool PUBLIC inc)

target_compile_features(memory_pool PUBLIC cxx_std_23)

target_compile_options(memory_pool PRIVATE -Wall -Wextra -Werror)