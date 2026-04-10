#include <benchmark/benchmark.h>

#include "Allocator.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace memorypool;

namespace {

constexpr std::array<int, 11> kBenchmarkSizes{
    8, 16, 32, 64, 128, 256, 512, 1024, 4096, 65536, 262144};
constexpr int kBatchSize = 512;

template <typename AllocateFn, typename DeallocateFn>
void RunSingleObjectLoop(benchmark::State& state, AllocateFn allocate_fn, DeallocateFn deallocate_fn) {
    const size_t size = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        void* ptr = allocate_fn(size);
        benchmark::DoNotOptimize(ptr);
        deallocate_fn(ptr);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunBatchLoop(benchmark::State& state, AllocateFn allocate_fn, DeallocateFn deallocate_fn) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<void*> ptrs;
    ptrs.reserve(kBatchSize);

    for (auto _ : state) {
        ptrs.clear();
        for (int i = 0; i < kBatchSize; ++i) {
            ptrs.push_back(allocate_fn(size));
        }
        benchmark::ClobberMemory();
        for (void* ptr : ptrs) {
            deallocate_fn(ptr);
        }
    }

    state.SetItemsProcessed(state.iterations() * kBatchSize);
    state.SetBytesProcessed(state.iterations() * kBatchSize * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunMixedSizeLoop(benchmark::State& state, AllocateFn allocate_fn, DeallocateFn deallocate_fn) {
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(kBatchSize);

    for (auto _ : state) {
        live.clear();
        for (int i = 0; i < kBatchSize; ++i) {
            size_t size = static_cast<size_t>(kBenchmarkSizes[static_cast<size_t>(i) % kBenchmarkSizes.size()]);
            void* ptr = allocate_fn(size);
            benchmark::DoNotOptimize(ptr);
            std::memset(ptr, i, size);
            live.emplace_back(ptr, size);

            if ((i % 4) == 3) {
                deallocate_fn(live.back().first);
                live.pop_back();
            }
        }

        for (auto it = live.rbegin(); it != live.rend(); ++it) {
            deallocate_fn(it->first);
        }
    }

    state.SetItemsProcessed(state.iterations() * kBatchSize);
}

static void BM_MallocAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocBatch(benchmark::State& state) {
    RunBatchLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolBatch(benchmark::State& state) {
    RunBatchLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocMixedSizes(benchmark::State& state) {
    RunMixedSizeLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolMixedSizes(benchmark::State& state) {
    RunMixedSizeLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); });
}

static void BM_MemoryPoolDirectAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); });
}

BENCHMARK(BM_MallocAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();
BENCHMARK(BM_MemoryPoolAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();

BENCHMARK(BM_MallocBatch)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();
BENCHMARK(BM_MemoryPoolBatch)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();

BENCHMARK(BM_MallocAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_MemoryPoolAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->ThreadRange(2, 8)->UseRealTime();

BENCHMARK(BM_MallocMixedSizes)->Threads(1)->UseRealTime();
BENCHMARK(BM_MemoryPoolMixedSizes)->Threads(1)->UseRealTime();
BENCHMARK(BM_MallocMixedSizes)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_MemoryPoolMixedSizes)->ThreadRange(2, 8)->UseRealTime();

BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(262152)->UseRealTime();
BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(524288)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
