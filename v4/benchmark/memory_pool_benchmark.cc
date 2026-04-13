#include <benchmark/benchmark.h>

#include "Allocator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace memorypool;

namespace {

constexpr std::array<int, 11> kBenchmarkSizes{
    8, 16, 32, 64, 128, 256, 512, 1024, 4096, 65536, 262144};
constexpr std::array<int, 6> kMetricSizes{13, 63, 127, 255, 4096, 65536};
constexpr std::array<int, 11> kMetricMixedSizes{
    13, 27, 63, 95, 127, 191, 255, 256, 512, 4096, 65536};
constexpr int kBatchSize = 512;
constexpr size_t kTargetMetricLiveBytes = 8ULL * 1024 * 1024;

struct MemorySample {
    size_t rssBytes = 0;
    size_t vmHwmBytes = 0;
};

struct MetricSnapshot {
    size_t requestedBytes = 0;
    size_t reservedBytesEstimate = 0;
    size_t liveRequestedBytes = 0;
    size_t rssBefore = 0;
    size_t rssPeak = 0;
    size_t rssRetained = 0;
    size_t rssAfterRelease = 0;
    size_t vmHwm = 0;
    size_t batchSize = 0;
};

size_t ParseProcStatusBytes(std::string_view key) {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind(key, 0) != 0) {
            continue;
        }

        const size_t firstDigit = line.find_first_of("0123456789");
        if (firstDigit == std::string::npos) {
            return 0;
        }

        const size_t lastDigit = line.find_first_not_of("0123456789", firstDigit);
        return static_cast<size_t>(std::stoull(line.substr(firstDigit, lastDigit - firstDigit))) * 1024ULL;
    }
    return 0;
}

MemorySample ReadMemorySample() {
    return {
        ParseProcStatusBytes("VmRSS:"),
        ParseProcStatusBytes("VmHWM:"),
    };
}

size_t EstimateMemoryPoolReservedBytes(size_t size) {
    size_t normalizedSize = 0;
    if (!normalizeSizeChecked(size, normalizedSize)) {
        return 0;
    }
    if (normalizedSize <= MAX_BYTES) {
        return normalizedSize;
    }

    const size_t pageCount = (normalizedSize + PAGE_SIZE - 1) / PAGE_SIZE;
    return pageCount * PAGE_SIZE;
}

size_t EstimateMallocReservedBytes(size_t size) {
    return size == 0 ? ALIGNLEN : size;
}

size_t GetMetricBatchSize(size_t size) {
    const size_t safeSize = std::max<size_t>(size, 1);
    return std::clamp<size_t>(kTargetMetricLiveBytes / safeSize, 32, kBatchSize);
}

void SetMetricCounters(benchmark::State& state,
                       const MetricSnapshot& totals,
                       size_t iterations) {
    if (iterations == 0) {
        return;
    }

    const double scale = 1.0 / static_cast<double>(iterations);
    const double avgRequested = static_cast<double>(totals.requestedBytes) * scale;
    const double avgReserved = static_cast<double>(totals.reservedBytesEstimate) * scale;
    const double avgLiveRequested = static_cast<double>(totals.liveRequestedBytes) * scale;
    const double avgRssBefore = static_cast<double>(totals.rssBefore) * scale;
    const double avgRssPeak = static_cast<double>(totals.rssPeak) * scale;
    const double avgRssRetained = static_cast<double>(totals.rssRetained) * scale;
    const double avgRssAfterRelease = static_cast<double>(totals.rssAfterRelease) * scale;
    const double avgVmHwm = static_cast<double>(totals.vmHwm) * scale;
    const double avgBatchSize = static_cast<double>(totals.batchSize) * scale;

    const double avgPeakDelta = std::max(0.0, avgRssPeak - avgRssBefore);
    const double avgRetainedDelta = std::max(0.0, avgRssRetained - avgRssBefore);
    const double avgReleaseDelta = std::max(0.0, avgRssAfterRelease - avgRssBefore);

    const double utilizationDenom = std::max(avgPeakDelta, avgReserved);
    const double utilization = utilizationDenom > 0.0 ? (avgRequested / utilizationDenom) : 0.0;
    const double internalFragmentation =
        avgReserved > 0.0 ? std::max(0.0, (avgReserved - avgRequested) / avgReserved) : 0.0;
    const double externalFragmentation =
        avgRetainedDelta > 0.0
            ? std::max(0.0, (avgRetainedDelta - avgLiveRequested) / avgRetainedDelta)
            : 0.0;
    const double reclaimRatio =
        avgPeakDelta > 0.0 ? std::max(0.0, (avgPeakDelta - avgReleaseDelta) / avgPeakDelta) : 0.0;

    state.counters["batch_size"] = avgBatchSize;
    state.counters["requested_bytes"] = avgRequested;
    state.counters["reserved_bytes_est"] = avgReserved;
    state.counters["live_requested_bytes"] = avgLiveRequested;
    state.counters["rss_before_kb"] = avgRssBefore / 1024.0;
    state.counters["vm_hwm_kb"] = avgVmHwm / 1024.0;
    state.counters["rss_peak_delta_kb"] = avgPeakDelta / 1024.0;
    state.counters["rss_retained_delta_kb"] = avgRetainedDelta / 1024.0;
    state.counters["rss_after_release_delta_kb"] = avgReleaseDelta / 1024.0;
    state.counters["memory_utilization"] = utilization;
    state.counters["internal_fragmentation"] = internalFragmentation;
    state.counters["external_fragmentation_estimate"] = externalFragmentation;
    state.counters["rss_reclaim_ratio"] = reclaimRatio;
}

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
void RunWarmSingleObjectLoop(benchmark::State& state, AllocateFn allocate_fn, DeallocateFn deallocate_fn) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<void*> warmup;
    warmup.reserve(kBatchSize);

    bool warmed = false;
    for (auto _ : state) {
        if (!warmed) {
            state.PauseTiming();
            for (int i = 0; i < kBatchSize; ++i) {
                void* ptr = allocate_fn(size);
                if (ptr == nullptr) {
                    state.SkipWithError("warmup allocation failed");
                    for (void* livePtr : warmup) {
                        deallocate_fn(livePtr);
                    }
                    return;
                }
                warmup.push_back(ptr);
            }
            for (void* ptr : warmup) {
                deallocate_fn(ptr);
            }
            warmed = true;
            state.ResumeTiming();
        }
        void* ptr = allocate_fn(size);
        benchmark::DoNotOptimize(ptr);
        deallocate_fn(ptr);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunMixedSizeLoop(benchmark::State& state, AllocateFn allocate_fn, DeallocateFn deallocate_fn) {
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(kBatchSize);

    for (auto _ : state) {
        live.clear();
        for (int i = 0; i < kBatchSize; ++i) {
            const size_t size = static_cast<size_t>(kBenchmarkSizes[static_cast<size_t>(i) % kBenchmarkSizes.size()]);
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

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunSingleSizeMetricLoop(benchmark::State& state,
                             AllocateFn allocate_fn,
                             DeallocateFn deallocate_fn,
                             ReserveFn reserve_fn) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t metricBatchSize = GetMetricBatchSize(size);
    std::vector<void*> ptrs;
    std::vector<void*> retained;
    ptrs.reserve(metricBatchSize);
    retained.reserve(metricBatchSize / 2 + 1);

    MetricSnapshot totals{};

    for (auto _ : state) {
        ptrs.clear();
        retained.clear();

        state.PauseTiming();
        const MemorySample before = ReadMemorySample();
        state.ResumeTiming();

        size_t requestedBytes = 0;
        size_t reservedBytes = 0;

        for (size_t i = 0; i < metricBatchSize; ++i) {
            void* ptr = allocate_fn(size);
            if (ptr == nullptr) {
                state.SkipWithError("allocation failed");
                for (void* livePtr : ptrs) {
                    deallocate_fn(livePtr);
                }
                return;
            }
            std::memset(ptr, static_cast<int>(i), size);
            ptrs.push_back(ptr);
            requestedBytes += size;
            reservedBytes += reserve_fn(size);
        }

        state.PauseTiming();
        const MemorySample peakSample = ReadMemorySample();
        state.ResumeTiming();

        for (size_t i = 0; i < ptrs.size(); ++i) {
            if ((i % 2) == 0) {
                retained.push_back(ptrs[i]);
            } else {
                deallocate_fn(ptrs[i]);
            }
        }

        state.PauseTiming();
        const MemorySample retainedSample = ReadMemorySample();
        state.ResumeTiming();

        for (void* ptr : retained) {
            deallocate_fn(ptr);
        }

        state.PauseTiming();
        const MemorySample afterRelease = ReadMemorySample();
        state.ResumeTiming();

        totals.requestedBytes += requestedBytes;
        totals.reservedBytesEstimate += reservedBytes;
        totals.liveRequestedBytes += retained.size() * size;
        totals.rssBefore += before.rssBytes;
        totals.rssPeak += std::max({before.rssBytes, peakSample.rssBytes, retainedSample.rssBytes, afterRelease.rssBytes});
        totals.rssRetained += retainedSample.rssBytes;
        totals.rssAfterRelease += afterRelease.rssBytes;
        totals.vmHwm += std::max({before.vmHwmBytes, peakSample.vmHwmBytes, retainedSample.vmHwmBytes, afterRelease.vmHwmBytes});
        totals.batchSize += metricBatchSize;
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(metricBatchSize));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(metricBatchSize) * static_cast<int64_t>(size));
    SetMetricCounters(state, totals, static_cast<size_t>(state.iterations()));
}

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunMixedMetricLoop(benchmark::State& state,
                        AllocateFn allocate_fn,
                        DeallocateFn deallocate_fn,
                        ReserveFn reserve_fn) {
    std::vector<std::pair<void*, size_t>> ptrs;
    std::vector<std::pair<void*, size_t>> retained;
    ptrs.reserve(kBatchSize);
    retained.reserve(kBatchSize / 2 + 1);

    MetricSnapshot totals{};

    for (auto _ : state) {
        ptrs.clear();
        retained.clear();

        state.PauseTiming();
        const MemorySample before = ReadMemorySample();
        state.ResumeTiming();

        size_t requestedBytes = 0;
        size_t reservedBytes = 0;

        for (int i = 0; i < kBatchSize; ++i) {
            const size_t size = static_cast<size_t>(kMetricMixedSizes[static_cast<size_t>(i) % kMetricMixedSizes.size()]);
            void* ptr = allocate_fn(size);
            if (ptr == nullptr) {
                state.SkipWithError("allocation failed");
                for (auto& entry : ptrs) {
                    deallocate_fn(entry.first);
                }
                return;
            }
            std::memset(ptr, i, size);
            ptrs.emplace_back(ptr, size);
            requestedBytes += size;
            reservedBytes += reserve_fn(size);
        }

        state.PauseTiming();
        const MemorySample peakSample = ReadMemorySample();
        state.ResumeTiming();

        for (size_t i = 0; i < ptrs.size(); ++i) {
            if ((i % 2) == 0) {
                retained.push_back(ptrs[i]);
            } else {
                deallocate_fn(ptrs[i].first);
            }
        }

        state.PauseTiming();
        const MemorySample retainedSample = ReadMemorySample();
        state.ResumeTiming();

        size_t liveRequestedBytes = 0;
        for (auto& entry : retained) {
            liveRequestedBytes += entry.second;
            deallocate_fn(entry.first);
        }

        state.PauseTiming();
        const MemorySample afterRelease = ReadMemorySample();
        state.ResumeTiming();

        totals.requestedBytes += requestedBytes;
        totals.reservedBytesEstimate += reservedBytes;
        totals.liveRequestedBytes += liveRequestedBytes;
        totals.rssBefore += before.rssBytes;
        totals.rssPeak += std::max({before.rssBytes, peakSample.rssBytes, retainedSample.rssBytes, afterRelease.rssBytes});
        totals.rssRetained += retainedSample.rssBytes;
        totals.rssAfterRelease += afterRelease.rssBytes;
        totals.vmHwm += std::max({before.vmHwmBytes, peakSample.vmHwmBytes, retainedSample.vmHwmBytes, afterRelease.vmHwmBytes});
        totals.batchSize += kBatchSize;
    }

    state.SetItemsProcessed(state.iterations() * kBatchSize);
    SetMetricCounters(state, totals, static_cast<size_t>(state.iterations()));
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

static void BM_MallocWarmAllocateFree(benchmark::State& state) {
    RunWarmSingleObjectLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolWarmAllocateFree(benchmark::State& state) {
    RunWarmSingleObjectLoop(
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

static void BM_MallocSingleSizeMetrics(benchmark::State& state) {
    RunSingleSizeMetricLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); },
        [](size_t size) { return EstimateMallocReservedBytes(size); });
}

static void BM_MemoryPoolSingleSizeMetrics(benchmark::State& state) {
    RunSingleSizeMetricLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); },
        [](size_t size) { return EstimateMemoryPoolReservedBytes(size); });
}

static void BM_MallocMixedSizeMetrics(benchmark::State& state) {
    RunMixedMetricLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); },
        [](size_t size) { return EstimateMallocReservedBytes(size); });
}

static void BM_MemoryPoolMixedSizeMetrics(benchmark::State& state) {
    RunMixedMetricLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); },
        [](size_t size) { return EstimateMemoryPoolReservedBytes(size); });
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

BENCHMARK(BM_MallocWarmAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();
BENCHMARK(BM_MemoryPoolWarmAllocateFree)->ArgsProduct({benchmark::CreateRange(8, 262144, 2)})->UseRealTime();

BENCHMARK(BM_MallocMixedSizes)->Threads(1)->UseRealTime();
BENCHMARK(BM_MemoryPoolMixedSizes)->Threads(1)->UseRealTime();
BENCHMARK(BM_MallocMixedSizes)->ThreadRange(2, 8)->UseRealTime();
BENCHMARK(BM_MemoryPoolMixedSizes)->ThreadRange(2, 8)->UseRealTime();

BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[0])->UseRealTime();
BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[1])->UseRealTime();
BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[2])->UseRealTime();
BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[3])->UseRealTime();
BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[4])->UseRealTime();
BENCHMARK(BM_MallocSingleSizeMetrics)->Arg(kMetricSizes[5])->UseRealTime();

BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[0])->UseRealTime();
BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[1])->UseRealTime();
BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[2])->UseRealTime();
BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[3])->UseRealTime();
BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[4])->UseRealTime();
BENCHMARK(BM_MemoryPoolSingleSizeMetrics)->Arg(kMetricSizes[5])->UseRealTime();

BENCHMARK(BM_MallocMixedSizeMetrics)->Threads(1)->UseRealTime();
BENCHMARK(BM_MemoryPoolMixedSizeMetrics)->Threads(1)->UseRealTime();

BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(262152)->UseRealTime();
BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(524288)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
