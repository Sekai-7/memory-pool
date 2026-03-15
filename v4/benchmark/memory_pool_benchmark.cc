#include <benchmark/benchmark.h>
#include "Allocator.h"
#include <cstdlib>
#include <vector>

using namespace memorypool;

// --- Single Thread Malloc ---
static void BM_Malloc_SingleThread(benchmark::State& state) {
    const size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = std::malloc(size);
        benchmark::DoNotOptimize(ptr);
        std::free(ptr);
    }
}
BENCHMARK(BM_Malloc_SingleThread)->RangeMultiplier(2)->Range(8, 1024);

// --- Single Thread MemoryPool ---
static void BM_MemoryPool_SingleThread(benchmark::State& state) {
    const size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = allocate(size);
        benchmark::DoNotOptimize(ptr);
        deallocate(ptr, size);
    }
}
BENCHMARK(BM_MemoryPool_SingleThread)->RangeMultiplier(2)->Range(8, 1024);

// --- Multi Thread Malloc ---
static void BM_Malloc_MultiThread(benchmark::State& state) {
    const size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = std::malloc(size);
        benchmark::DoNotOptimize(ptr);
        std::free(ptr);
    }
}
BENCHMARK(BM_Malloc_MultiThread)->RangeMultiplier(2)->Range(8, 1024)->Threads(2)->Threads(4)->Threads(8);

// --- Multi Thread MemoryPool ---
static void BM_MemoryPool_MultiThread(benchmark::State& state) {
    const size_t size = state.range(0);
    for (auto _ : state) {
        void* ptr = allocate(size);
        benchmark::DoNotOptimize(ptr);
        deallocate(ptr, size);
    }
}
BENCHMARK(BM_MemoryPool_MultiThread)->RangeMultiplier(2)->Range(8, 1024)->Threads(2)->Threads(4)->Threads(8);

// --- Batch Malloc ---
static void BM_Malloc_Batch(benchmark::State& state) {
    const size_t size = state.range(0);
    const int batch_size = 1000;
    std::vector<void*> ptrs;
    ptrs.reserve(batch_size);
    for (auto _ : state) {
        for (int i = 0; i < batch_size; ++i) {
            ptrs.push_back(std::malloc(size));
        }
        for (void* ptr : ptrs) {
            std::free(ptr);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_Malloc_Batch)->RangeMultiplier(2)->Range(8, 1024);

// --- Batch MemoryPool ---
static void BM_MemoryPool_Batch(benchmark::State& state) {
    const size_t size = state.range(0);
    const int batch_size = 1000;
    std::vector<void*> ptrs;
    ptrs.reserve(batch_size);
    for (auto _ : state) {
        for (int i = 0; i < batch_size; ++i) {
            ptrs.push_back(allocate(size));
        }
        for (void* ptr : ptrs) {
            deallocate(ptr, size);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MemoryPool_Batch)->RangeMultiplier(2)->Range(8, 1024);

// --- Batch Multi Thread Malloc ---
static void BM_Malloc_Batch_MultiThread(benchmark::State& state) {
    const size_t size = state.range(0);
    const int batch_size = 1000;
    std::vector<void*> ptrs;
    ptrs.reserve(batch_size);
    for (auto _ : state) {
        for (int i = 0; i < batch_size; ++i) {
            ptrs.push_back(std::malloc(size));
        }
        for (void* ptr : ptrs) {
            std::free(ptr);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_Malloc_Batch_MultiThread)->RangeMultiplier(2)->Range(8, 1024)->Threads(2)->Threads(4)->Threads(8);

// --- Batch Multi Thread MemoryPool ---
static void BM_MemoryPool_Batch_MultiThread(benchmark::State& state) {
    const size_t size = state.range(0);
    const int batch_size = 1000;
    std::vector<void*> ptrs;
    ptrs.reserve(batch_size);
    for (auto _ : state) {
        for (int i = 0; i < batch_size; ++i) {
            ptrs.push_back(allocate(size));
        }
        for (void* ptr : ptrs) {
            deallocate(ptr, size);
        }
        ptrs.clear();
    }
}
BENCHMARK(BM_MemoryPool_Batch_MultiThread)->RangeMultiplier(2)->Range(8, 1024)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
