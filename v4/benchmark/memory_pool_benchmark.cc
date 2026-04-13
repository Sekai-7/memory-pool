#include <benchmark/benchmark.h>

#include "Allocator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace memorypool;

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::array<int, 11> kBenchmarkSizes{
    8, 16, 32, 64, 128, 256, 512, 1024, 4096, 65536, 262144};
constexpr std::array<int, 6> kMetricSizes{13, 63, 127, 255, 4096, 65536};
constexpr std::array<int, 11> kMetricMixedSizes{
    13, 27, 63, 95, 127, 191, 255, 256, 512, 4096, 65536};
constexpr std::array<int, 3> kSharedContentionSizes{64, 256, 4096};
constexpr std::array<int, 3> kRssSizes{256, 4096, 65536};
constexpr std::array<int, 3> kLatencySizes{64, 256, 4096};

constexpr int kBatchSize = 512;
constexpr size_t kTargetMetricLiveBytes = 8ULL * 1024 * 1024;
constexpr size_t kTargetRssBytes = 64ULL * 1024 * 1024;
constexpr size_t kTailLatencySamples = 50000;
constexpr size_t kSlowPathSamples = 5000;
constexpr std::chrono::milliseconds kRssSettleDelay(20);

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

struct ThreadBarrier {
    void wait(size_t participants) {
        std::unique_lock<std::mutex> lock(mutex);
        const size_t currentGeneration = generation;
        if (++arrived == participants) {
            arrived = 0;
            ++generation;
            cv.notify_all();
            return;
        }
        cv.wait(lock, [&] { return generation != currentGeneration; });
    }

    std::mutex mutex;
    std::condition_variable cv;
    size_t arrived = 0;
    size_t generation = 0;
};

struct CrossThreadHandoff {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<void*> batch;
    bool ready = false;
    double measuredSeconds = 0.0;
};

ThreadBarrier& GetBarrier(size_t threads) {
    static std::array<ThreadBarrier, 9> barriers{};
    return barriers.at(threads);
}

CrossThreadHandoff& GetCrossThreadHandoff() {
    static CrossThreadHandoff handoff;
    return handoff;
}

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

size_t GetRssBatchSize(size_t size) {
    const size_t safeSize = std::max<size_t>(size, 1);
    return std::clamp<size_t>(kTargetRssBytes / safeSize, 256, 16384);
}

size_t GetContentionBatchSize(size_t size) {
    if (size <= 64) {
        return 4096;
    }
    if (size <= 256) {
        return 2048;
    }
    if (size <= 4096) {
        return 1024;
    }
    return 256;
}

size_t PercentileIndex(size_t sampleCount, double percentile) {
    if (sampleCount == 0) {
        return 0;
    }
    const double raw = percentile * static_cast<double>(sampleCount - 1);
    const size_t idx = static_cast<size_t>(raw + 0.999999);
    return std::min(idx, sampleCount - 1);
}

void SetLatencyCounters(benchmark::State& state,
                        std::vector<uint64_t>& samplesNs,
                        size_t operationsPerSample) {
    if (samplesNs.empty()) {
        return;
    }

    std::sort(samplesNs.begin(), samplesNs.end());
    const auto avgNs =
        static_cast<double>(std::accumulate(samplesNs.begin(), samplesNs.end(), 0ULL)) /
        static_cast<double>(samplesNs.size());

    const double perOp = static_cast<double>(std::max<size_t>(operationsPerSample, 1));
    state.counters["latency_samples"] = static_cast<double>(samplesNs.size());
    state.counters["avg_ns"] = avgNs / perOp;
    state.counters["p50_ns"] = static_cast<double>(samplesNs[PercentileIndex(samplesNs.size(), 0.50)]) / perOp;
    state.counters["p90_ns"] = static_cast<double>(samplesNs[PercentileIndex(samplesNs.size(), 0.90)]) / perOp;
    state.counters["p99_ns"] = static_cast<double>(samplesNs[PercentileIndex(samplesNs.size(), 0.99)]) / perOp;
    state.counters["p99_9_ns"] = static_cast<double>(samplesNs[PercentileIndex(samplesNs.size(), 0.999)]) / perOp;
    state.counters["max_ns"] = static_cast<double>(samplesNs.back()) / perOp;
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

    const double avgGrowDelta = std::max(0.0, avgRssPeak - avgRssBefore);
    const double avgRetainedDelta = std::max(0.0, avgRssRetained - avgRssBefore);
    const double avgReleaseDelta = std::max(0.0, avgRssAfterRelease - avgRssBefore);

    const double utilizationDenom = std::max(avgGrowDelta, avgReserved);
    const double utilization = utilizationDenom > 0.0 ? (avgRequested / utilizationDenom) : 0.0;
    const double internalFragmentation =
        avgReserved > 0.0 ? std::max(0.0, (avgReserved - avgRequested) / avgReserved) : 0.0;
    const double externalFragmentation =
        avgRetainedDelta > 0.0
            ? std::max(0.0, (avgRetainedDelta - avgLiveRequested) / avgRetainedDelta)
            : 0.0;
    const double reclaimRatio =
        avgGrowDelta > 0.0 ? std::max(0.0, (avgGrowDelta - avgReleaseDelta) / avgGrowDelta) : 0.0;

    state.counters["batch_size"] = avgBatchSize;
    state.counters["requested_bytes"] = avgRequested;
    state.counters["reserved_bytes_est"] = avgReserved;
    state.counters["live_requested_bytes"] = avgLiveRequested;
    state.counters["rss_before_kb"] = avgRssBefore / 1024.0;
    state.counters["vm_hwm_kb"] = avgVmHwm / 1024.0;
    state.counters["rss_grow_delta_kb"] = avgGrowDelta / 1024.0;
    state.counters["rss_retained_delta_kb"] = avgRetainedDelta / 1024.0;
    state.counters["rss_after_release_delta_kb"] = avgReleaseDelta / 1024.0;
    state.counters["memory_utilization"] = utilization;
    state.counters["internal_fragmentation"] = internalFragmentation;
    state.counters["external_fragmentation_estimate"] = externalFragmentation;
    state.counters["rss_reclaim_ratio"] = reclaimRatio;
}

template <typename AllocateFn, typename DeallocateFn>
void RunSingleObjectLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    for (auto _ : state) {
        void* ptr = allocateFn(size);
        benchmark::DoNotOptimize(ptr);
        deallocateFn(ptr);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunBatchLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<void*> ptrs;
    ptrs.reserve(kBatchSize);

    for (auto _ : state) {
        ptrs.clear();
        for (int i = 0; i < kBatchSize; ++i) {
            ptrs.push_back(allocateFn(size));
        }
        benchmark::ClobberMemory();
        for (void* ptr : ptrs) {
            deallocateFn(ptr);
        }
    }

    state.SetItemsProcessed(state.iterations() * kBatchSize);
    state.SetBytesProcessed(state.iterations() * kBatchSize * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunWarmSingleObjectLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<void*> warmup;
    warmup.reserve(kBatchSize);

    bool warmed = false;
    for (auto _ : state) {
        if (!warmed) {
            state.PauseTiming();
            for (int i = 0; i < kBatchSize; ++i) {
                void* ptr = allocateFn(size);
                if (ptr == nullptr) {
                    state.SkipWithError("warmup allocation failed");
                    for (void* livePtr : warmup) {
                        deallocateFn(livePtr);
                    }
                    return;
                }
                warmup.push_back(ptr);
            }
            for (void* ptr : warmup) {
                deallocateFn(ptr);
            }
            warmed = true;
            state.ResumeTiming();
        }

        void* ptr = allocateFn(size);
        benchmark::DoNotOptimize(ptr);
        deallocateFn(ptr);
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

template <typename AllocateFn, typename DeallocateFn>
void RunMixedSizeLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    std::vector<std::pair<void*, size_t>> live;
    live.reserve(kBatchSize);

    for (auto _ : state) {
        live.clear();
        for (int i = 0; i < kBatchSize; ++i) {
            const size_t size = static_cast<size_t>(kBenchmarkSizes[static_cast<size_t>(i) % kBenchmarkSizes.size()]);
            void* ptr = allocateFn(size);
            benchmark::DoNotOptimize(ptr);
            std::memset(ptr, i, size);
            live.emplace_back(ptr, size);

            if ((i % 4) == 3) {
                deallocateFn(live.back().first);
                live.pop_back();
            }
        }

        for (auto it = live.rbegin(); it != live.rend(); ++it) {
            deallocateFn(it->first);
        }
    }

    state.SetItemsProcessed(state.iterations() * kBatchSize);
}

template <typename AllocateFn, typename DeallocateFn>
void RunSharedContentionLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t batchSize = GetContentionBatchSize(size);
    auto& barrier = GetBarrier(static_cast<size_t>(state.threads()));
    std::vector<void*> ptrs;
    ptrs.reserve(batchSize);

    for (auto _ : state) {
        barrier.wait(static_cast<size_t>(state.threads()));
        ptrs.clear();
        for (size_t i = 0; i < batchSize; ++i) {
            void* ptr = allocateFn(size);
            if (ptr == nullptr) {
                state.SkipWithError("allocation failed");
                return;
            }
            ptrs.push_back(ptr);
        }
        benchmark::ClobberMemory();
        for (void* ptr : ptrs) {
            deallocateFn(ptr);
        }
        barrier.wait(static_cast<size_t>(state.threads()));
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batchSize));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(batchSize) * static_cast<int64_t>(size));
    state.counters["contention_batch_size"] = static_cast<double>(batchSize);
}

template <typename AllocateFn, typename DeallocateFn>
void RunCrossThreadFreeLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    if (state.threads() != 2) {
        state.SkipWithError("cross-thread free requires exactly 2 threads");
        return;
    }

    const size_t size = static_cast<size_t>(state.range(0));
    const size_t batchSize = GetContentionBatchSize(size);
    auto& barrier = GetBarrier(2);
    auto& handoff = GetCrossThreadHandoff();

    state.PauseTiming();
    barrier.wait(2);
    if (state.thread_index() == 0) {
        std::lock_guard<std::mutex> lock(handoff.mutex);
        handoff.batch.clear();
        handoff.ready = false;
        handoff.measuredSeconds = 0.0;
    }
    barrier.wait(2);
    state.ResumeTiming();

    double measuredSeconds = 0.0;

    for (auto _ : state) {
        if (state.thread_index() == 0) {
            const auto start = Clock::now();
            std::vector<void*> produced;
            produced.reserve(batchSize);
            for (size_t i = 0; i < batchSize; ++i) {
                void* ptr = allocateFn(size);
                if (ptr == nullptr) {
                    state.SkipWithError("allocation failed");
                    return;
                }
                produced.push_back(ptr);
            }

            std::unique_lock<std::mutex> lock(handoff.mutex);
            handoff.cv.wait(lock, [&] { return !handoff.ready; });
            handoff.batch = std::move(produced);
            handoff.ready = true;
            handoff.cv.notify_all();
            handoff.cv.wait(lock, [&] { return !handoff.ready; });
            const auto end = Clock::now();
            measuredSeconds += std::chrono::duration<double>(end - start).count();
        } else {
            std::vector<void*> local;
            {
                std::unique_lock<std::mutex> lock(handoff.mutex);
                handoff.cv.wait(lock, [&] { return handoff.ready; });
                local = std::move(handoff.batch);
                handoff.ready = false;
                handoff.cv.notify_all();
            }
            for (void* ptr : local) {
                deallocateFn(ptr);
            }
        }
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batchSize));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(batchSize) * static_cast<int64_t>(size));
    state.counters["handoff_batch_size"] = static_cast<double>(batchSize);
    state.counters["cross_thread_avg_ns"] =
        (measuredSeconds * 1e9) / static_cast<double>(std::max<size_t>(state.iterations() * batchSize, 1));

    state.PauseTiming();
    barrier.wait(2);
    if (state.thread_index() == 0) {
        std::lock_guard<std::mutex> lock(handoff.mutex);
        handoff.measuredSeconds = measuredSeconds;
    }
    barrier.wait(2);
    double sharedMeasuredSeconds = 0.0;
    {
        std::lock_guard<std::mutex> lock(handoff.mutex);
        sharedMeasuredSeconds = handoff.measuredSeconds;
    }
    state.ResumeTiming();

    state.SetIterationTime(sharedMeasuredSeconds);
}

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunMetricLoop(benchmark::State& state,
                   const std::vector<size_t>& sizes,
                   size_t batchSize,
                   size_t retainStride,
                   AllocateFn allocateFn,
                   DeallocateFn deallocateFn,
                   ReserveFn reserveFn) {
    std::vector<std::pair<void*, size_t>> ptrs;
    std::vector<std::pair<void*, size_t>> retained;
    ptrs.reserve(batchSize);
    retained.reserve(batchSize / 2 + 1);

    MetricSnapshot totals{};

    for (auto _ : state) {
        ptrs.clear();
        retained.clear();

        state.PauseTiming();
        const MemorySample before = ReadMemorySample();
        state.ResumeTiming();

        size_t requestedBytes = 0;
        size_t reservedBytes = 0;
        for (size_t i = 0; i < batchSize; ++i) {
            const size_t size = sizes[i % sizes.size()];
            void* ptr = allocateFn(size);
            if (ptr == nullptr) {
                state.SkipWithError("allocation failed");
                for (auto& entry : ptrs) {
                    deallocateFn(entry.first);
                }
                return;
            }
            std::memset(ptr, static_cast<int>(i), size);
            ptrs.emplace_back(ptr, size);
            requestedBytes += size;
            reservedBytes += reserveFn(size);
        }

        state.PauseTiming();
        const MemorySample peakSample = ReadMemorySample();
        state.ResumeTiming();

        for (size_t i = 0; i < ptrs.size(); ++i) {
            if ((i % retainStride) == 0) {
                retained.push_back(ptrs[i]);
            } else {
                deallocateFn(ptrs[i].first);
            }
        }

        state.PauseTiming();
        std::this_thread::sleep_for(kRssSettleDelay);
        const MemorySample retainedSample = ReadMemorySample();
        state.ResumeTiming();

        size_t liveRequestedBytes = 0;
        for (auto& entry : retained) {
            liveRequestedBytes += entry.second;
            deallocateFn(entry.first);
        }

        state.PauseTiming();
        std::this_thread::sleep_for(kRssSettleDelay);
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
        totals.batchSize += batchSize;
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batchSize));
    SetMetricCounters(state, totals, static_cast<size_t>(state.iterations()));
}

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunSingleSizeMetricLoop(benchmark::State& state,
                             AllocateFn allocateFn,
                             DeallocateFn deallocateFn,
                             ReserveFn reserveFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t batchSize = GetMetricBatchSize(size);
    RunMetricLoop(state, {size}, batchSize, 2, allocateFn, deallocateFn, reserveFn);
}

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunMixedMetricLoop(benchmark::State& state,
                        AllocateFn allocateFn,
                        DeallocateFn deallocateFn,
                        ReserveFn reserveFn) {
    std::vector<size_t> sizes;
    sizes.reserve(kMetricMixedSizes.size());
    for (int size : kMetricMixedSizes) {
        sizes.push_back(static_cast<size_t>(size));
    }
    RunMetricLoop(state, sizes, kBatchSize, 2, allocateFn, deallocateFn, reserveFn);
}

template <typename AllocateFn, typename DeallocateFn, typename ReserveFn>
void RunRssLifecycleLoop(benchmark::State& state,
                         AllocateFn allocateFn,
                         DeallocateFn deallocateFn,
                         ReserveFn reserveFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t batchSize = GetRssBatchSize(size);
    RunMetricLoop(state, {size}, batchSize, 4, allocateFn, deallocateFn, reserveFn);
}

template <typename AllocateFn, typename DeallocateFn>
void RunHotTailLatencyLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<void*> warmup;
    warmup.reserve(kBatchSize);
    std::vector<uint64_t> samplesNs;
    samplesNs.reserve(kTailLatencySamples);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < kBatchSize; ++i) {
            void* ptr = allocateFn(size);
            if (ptr == nullptr) {
                state.SkipWithError("warmup allocation failed");
                for (void* livePtr : warmup) {
                    deallocateFn(livePtr);
                }
                return;
            }
            warmup.push_back(ptr);
        }
        for (void* ptr : warmup) {
            deallocateFn(ptr);
        }
        warmup.clear();
        state.ResumeTiming();

        samplesNs.clear();
        for (size_t i = 0; i < kTailLatencySamples; ++i) {
            const auto start = Clock::now();
            void* ptr = allocateFn(size);
            deallocateFn(ptr);
            const auto end = Clock::now();
            samplesNs.push_back(static_cast<uint64_t>(std::chrono::duration_cast<Nanoseconds>(end - start).count()));
        }
    }

    SetLatencyCounters(state, samplesNs, 1);
}

template <typename AllocateFn, typename DeallocateFn>
void RunSlowPathTailLatencyLoop(benchmark::State& state, AllocateFn allocateFn, DeallocateFn deallocateFn) {
    const size_t size = static_cast<size_t>(state.range(0));
    const size_t batchSize = GetContentionBatchSize(size);
    std::vector<void*> ptrs;
    ptrs.reserve(batchSize);
    std::vector<uint64_t> samplesNs;
    samplesNs.reserve(kSlowPathSamples);

    for (auto _ : state) {
        samplesNs.clear();
        for (size_t sample = 0; sample < kSlowPathSamples; ++sample) {
            ptrs.clear();
            const auto start = Clock::now();
            for (size_t i = 0; i < batchSize; ++i) {
                void* ptr = allocateFn(size);
                if (ptr == nullptr) {
                    state.SkipWithError("allocation failed");
                    return;
                }
                ptrs.push_back(ptr);
            }
            for (void* ptr : ptrs) {
                deallocateFn(ptr);
            }
            const auto end = Clock::now();
            samplesNs.push_back(static_cast<uint64_t>(std::chrono::duration_cast<Nanoseconds>(end - start).count()));
        }
    }

    SetLatencyCounters(state, samplesNs, batchSize);
    state.counters["slow_path_batch_size"] = static_cast<double>(batchSize);
}

static void BM_MallocAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocBatch(benchmark::State& state) {
    RunBatchLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolBatch(benchmark::State& state) {
    RunBatchLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocWarmAllocateFree(benchmark::State& state) {
    RunWarmSingleObjectLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolWarmAllocateFree(benchmark::State& state) {
    RunWarmSingleObjectLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocMixedSizes(benchmark::State& state) {
    RunMixedSizeLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolMixedSizes(benchmark::State& state) {
    RunMixedSizeLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
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

static void BM_MallocRssLifecycle(benchmark::State& state) {
    RunRssLifecycleLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); },
        [](size_t size) { return EstimateMallocReservedBytes(size); });
}

static void BM_MemoryPoolRssLifecycle(benchmark::State& state) {
    RunRssLifecycleLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); },
        [](size_t size) { return EstimateMemoryPoolReservedBytes(size); });
}

static void BM_MallocSharedCentralContention(benchmark::State& state) {
    RunSharedContentionLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolSharedCentralContention(benchmark::State& state) {
    RunSharedContentionLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocCrossThreadFree(benchmark::State& state) {
    RunCrossThreadFreeLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolCrossThreadFree(benchmark::State& state) {
    RunCrossThreadFreeLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocHotTailLatency(benchmark::State& state) {
    RunHotTailLatencyLoop(state, [](size_t size) { return std::malloc(size); }, [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolHotTailLatency(benchmark::State& state) {
    RunHotTailLatencyLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
}

static void BM_MallocSlowPathTailLatency(benchmark::State& state) {
    RunSlowPathTailLatencyLoop(
        state,
        [](size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); });
}

static void BM_MemoryPoolSlowPathTailLatency(benchmark::State& state) {
    RunSlowPathTailLatencyLoop(
        state,
        [](size_t size) { return allocate(size); },
        [](void* ptr) { deallocate(ptr); });
}

static void BM_MemoryPoolDirectAllocateFree(benchmark::State& state) {
    RunSingleObjectLoop(state, [](size_t size) { return allocate(size); }, [](void* ptr) { deallocate(ptr); });
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

BENCHMARK(BM_MallocRssLifecycle)->Arg(kRssSizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocRssLifecycle)->Arg(kRssSizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocRssLifecycle)->Arg(kRssSizes[2])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolRssLifecycle)->Arg(kRssSizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolRssLifecycle)->Arg(kRssSizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolRssLifecycle)->Arg(kRssSizes[2])->UseRealTime()->Iterations(1);

BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[0])->Threads(2)->UseRealTime();
BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[0])->Threads(4)->UseRealTime();
BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[1])->Threads(2)->UseRealTime();
BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[1])->Threads(4)->UseRealTime();
BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[2])->Threads(2)->UseRealTime();
BENCHMARK(BM_MallocSharedCentralContention)->Arg(kSharedContentionSizes[2])->Threads(4)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[0])->Threads(2)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[0])->Threads(4)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[1])->Threads(2)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[1])->Threads(4)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[2])->Threads(2)->UseRealTime();
BENCHMARK(BM_MemoryPoolSharedCentralContention)->Arg(kSharedContentionSizes[2])->Threads(4)->UseRealTime();

BENCHMARK(BM_MallocCrossThreadFree)->Arg(kSharedContentionSizes[0])->Threads(2)->UseManualTime();
BENCHMARK(BM_MallocCrossThreadFree)->Arg(kSharedContentionSizes[1])->Threads(2)->UseManualTime();
BENCHMARK(BM_MallocCrossThreadFree)->Arg(kSharedContentionSizes[2])->Threads(2)->UseManualTime();
BENCHMARK(BM_MemoryPoolCrossThreadFree)->Arg(kSharedContentionSizes[0])->Threads(2)->UseManualTime();
BENCHMARK(BM_MemoryPoolCrossThreadFree)->Arg(kSharedContentionSizes[1])->Threads(2)->UseManualTime();
BENCHMARK(BM_MemoryPoolCrossThreadFree)->Arg(kSharedContentionSizes[2])->Threads(2)->UseManualTime();

BENCHMARK(BM_MallocHotTailLatency)->Arg(kLatencySizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocHotTailLatency)->Arg(kLatencySizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocHotTailLatency)->Arg(kLatencySizes[2])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolHotTailLatency)->Arg(kLatencySizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolHotTailLatency)->Arg(kLatencySizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolHotTailLatency)->Arg(kLatencySizes[2])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocSlowPathTailLatency)->Arg(kLatencySizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocSlowPathTailLatency)->Arg(kLatencySizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MallocSlowPathTailLatency)->Arg(kLatencySizes[2])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolSlowPathTailLatency)->Arg(kLatencySizes[0])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolSlowPathTailLatency)->Arg(kLatencySizes[1])->UseRealTime()->Iterations(1);
BENCHMARK(BM_MemoryPoolSlowPathTailLatency)->Arg(kLatencySizes[2])->UseRealTime()->Iterations(1);

BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(262152)->UseRealTime();
BENCHMARK(BM_MemoryPoolDirectAllocateFree)->Arg(524288)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
