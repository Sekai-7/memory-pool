#include <gtest/gtest.h>

#include "Allocator.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "util.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace memorypool;

namespace {

constexpr std::array<size_t, 11> kPooledSizes{
    8, 16, 64, 256, 512, 1024, 4096, 8192, 65536, 131072, 262144};
constexpr std::array<size_t, 2> kDirectSizes{262152, 524288};
constexpr size_t kPageCacheDrainSpanCount = 32;

class SetSpanFailureInjectionGuard {
public:
    ~SetSpanFailureInjectionGuard() {
        resetSetSpanFailureInjection();
    }
};

std::vector<Span*> ReserveFullPageCacheSpans(size_t count = kPageCacheDrainSpanCount) {
    std::vector<Span*> spans;
    spans.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Span* span = PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN);
        EXPECT_NE(span, nullptr);
        if (span == nullptr) {
            break;
        }
        spans.push_back(span);
    }
    return spans;
}

constexpr uint16_t referenceListIndex(size_t size) {
    if (size == 0) {
        return 0;
    }
    if (size <= MAX_SMALL_BYTES) {
        return static_cast<uint16_t>(align(size) >> 3) - 1;
    }
    size_t alignSize = std::bit_ceil(size);
    uint16_t baseIndex = static_cast<uint16_t>(MAX_SMALL_BYTES / ALIGNLEN);
    uint16_t shift = 63 - std::countl_zero(alignSize);
    return baseIndex + shift - 9;
}

}  // namespace

TEST(MemoryPoolTest, AllocatesSupportedSizeClasses) {
    for (size_t size : kPooledSizes) {
        void* ptr = allocate(size);
        ASSERT_NE(ptr, nullptr) << "size=" << size;
        deallocate(ptr);
    }
}

TEST(MemoryPoolTest, ReturnsAlignedPointers) {
    for (size_t size : kPooledSizes) {
        void* ptr = allocate(size);
        ASSERT_NE(ptr, nullptr) << "size=" << size;
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % ALIGNLEN, 0U) << "size=" << size;
        deallocate(ptr);
    }
}

TEST(MemoryPoolTest, RoutesLargeAllocationsDirectlyToPageCache) {
    for (size_t size : kDirectSizes) {
        void* ptr = allocate(size);
        ASSERT_NE(ptr, nullptr) << "size=" << size;

        Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptr));
        ASSERT_NE(span, nullptr) << "size=" << size;
        EXPECT_TRUE(span->isDirect) << "size=" << size;
        EXPECT_EQ(span->objSize, 0U) << "size=" << size;

        deallocate(ptr);
        EXPECT_EQ(RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptr)), nullptr)
            << "size=" << size;
    }
}

TEST(MemoryPoolTest, PreservesWrittenData) {
    constexpr size_t kSize = 128;

    auto* ptr = static_cast<unsigned char*>(allocate(kSize));
    ASSERT_NE(ptr, nullptr);

    for (size_t i = 0; i < kSize; ++i) {
        ptr[i] = static_cast<unsigned char>(i);
    }

    for (size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(ptr[i], static_cast<unsigned char>(i));
    }

    deallocate(ptr);
}

TEST(MemoryPoolTest, PreservesWrittenDataForDirectAllocation) {
    constexpr size_t kSize = 262152;

    auto* ptr = static_cast<unsigned char*>(allocate(kSize));
    ASSERT_NE(ptr, nullptr);

    std::memset(ptr, 0x5a, kSize);

    for (size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(ptr[i], 0x5a);
    }

    deallocate(ptr);
}

TEST(MemoryPoolTest, ReusesFreedBlocksWithinThread) {
    constexpr size_t kSize = 64;

    void* first = allocate(kSize);
    ASSERT_NE(first, nullptr);
    deallocate(first);

    void* second = allocate(kSize);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first, second);
    deallocate(second);
}

TEST(MemoryPoolTest, ThreadCacheKeepsStableLocalReuseWindow) {
    constexpr size_t kSize = 64;
    constexpr size_t kCount = 64;

    std::vector<void*> firstRound;
    firstRound.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        void* ptr = allocate(kSize);
        ASSERT_NE(ptr, nullptr);
        firstRound.push_back(ptr);
    }

    for (void* ptr : firstRound) {
        deallocate(ptr);
    }

    std::vector<void*> secondRound;
    secondRound.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        void* ptr = allocate(kSize);
        ASSERT_NE(ptr, nullptr);
        secondRound.push_back(ptr);
    }

    size_t reuseCount = 0;
    for (void* ptr : secondRound) {
        for (void* oldPtr : firstRound) {
            if (ptr == oldPtr) {
                ++reuseCount;
                break;
            }
        }
    }

    EXPECT_GE(reuseCount, kCount / 2);

    for (void* ptr : secondRound) {
        deallocate(ptr);
    }
}

TEST(MemoryPoolTest, SupportsRepeatedAllocationsAndReleases) {
    constexpr size_t kSize = 32;
    constexpr int kCount = 1000;

    std::vector<void*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        void* ptr = allocate(kSize);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    for (void* ptr : ptrs) {
        deallocate(ptr);
    }
}

TEST(MemoryPoolTest, HandlesConcurrentMixedSizeWorkload) {
    constexpr int kThreadCount = 4;
    constexpr int kOperationsPerThread = 2000;
    constexpr std::array<size_t, 5> kMixedSizes{64, 4096, 65536, 262144, 262152};

    std::atomic<bool> failed{false};

    auto worker = [&failed, &kMixedSizes](int seed) {
        std::vector<std::pair<void*, size_t>> live;
        live.reserve(kOperationsPerThread);

        for (int i = 0; i < kOperationsPerThread && !failed.load(); ++i) {
            size_t size = kMixedSizes[(static_cast<size_t>(seed) + static_cast<size_t>(i)) % kMixedSizes.size()];
            auto* ptr = static_cast<unsigned char*>(allocate(size));
            if (ptr == nullptr) {
                failed.store(true);
                break;
            }

            std::memset(ptr, seed + i, size);
            live.emplace_back(ptr, size);

            if ((i % 3) == 0) {
                deallocate(live.back().first);
                live.pop_back();
            }
        }

        for (auto it = live.rbegin(); it != live.rend(); ++it) {
            deallocate(it->first);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i) {
        threads.emplace_back(worker, i + 1);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(failed.load());
}

TEST(MemoryPoolTest, HandlesConcurrentDifferentSizeClassWorkload) {
    constexpr std::array<size_t, 4> kSizes{32, 64, 256, 4096};
    constexpr int kOperationsPerThread = 2000;

    std::atomic<bool> failed{false};

    auto worker = [&failed](size_t size) {
        std::vector<void*> ptrs;
        ptrs.reserve(kOperationsPerThread);

        for (int i = 0; i < kOperationsPerThread && !failed.load(); ++i) {
            void* ptr = allocate(size);
            if (ptr == nullptr) {
                failed.store(true);
                break;
            }
            ptrs.push_back(ptr);

            if ((i % 4) == 3) {
                deallocate(ptrs.back());
                ptrs.pop_back();
            }
        }

        for (auto it = ptrs.rbegin(); it != ptrs.rend(); ++it) {
            deallocate(*it);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kSizes.size());
    for (size_t size : kSizes) {
        threads.emplace_back(worker, size);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(failed.load());
}

TEST(MemoryPoolTest, ZeroByteAllocationsUseMinimumAlignedBlock) {
    void* ptr = allocate(0);
    ASSERT_NE(ptr, nullptr);

    Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptr));
    ASSERT_NE(span, nullptr);
    EXPECT_FALSE(span->isDirect);
    EXPECT_EQ(span->objSize, ALIGNLEN);

    deallocate(ptr);
}

TEST(MemoryPoolTest, RejectsSizesThatOverflowNormalizationOrPageCount) {
    EXPECT_EQ(allocate(std::numeric_limits<size_t>::max()), nullptr);
    EXPECT_EQ(allocate(std::numeric_limits<size_t>::max() - ALIGNLEN), nullptr);
}

TEST(MemoryPoolTest, ListIndexLookupMatchesReferenceMapping) {
    for (size_t size = 1; size <= MAX_BYTES; ++size) {
        EXPECT_EQ(getListIndex(size), referenceListIndex(size)) << "size=" << size;
    }
}

TEST(MemoryPoolTest, ClassSizeLookupCoversEveryListIndex) {
    for (size_t index = 0; index < FREE_LIST_SIZE; ++index) {
        const size_t classSize = getClassSize(index);
        ASSERT_NE(classSize, 0U) << "index=" << index;
        EXPECT_EQ(getListIndex(classSize), index) << "classSize=" << classSize;
    }
}

TEST(MemoryPoolTest, ClassSizeIsLargeEnoughForEveryPooledRequest) {
    for (size_t size = 1; size <= MAX_BYTES; ++size) {
        size_t normalizedSize = 0;
        ASSERT_TRUE(normalizeSizeChecked(size, normalizedSize)) << "size=" << size;
        const size_t classSize = getClassSize(getListIndex(normalizedSize));
        EXPECT_GE(classSize, normalizedSize) << "size=" << size;
    }
}

TEST(MemoryPoolTest, CoarseBucketsSplitSpansByStandardClassSize) {
    struct Case {
        size_t requestSize;
        size_t classSize;
    };

    constexpr std::array<Case, 3> cases{{
        {MAX_SMALL_BYTES + 1, 512},
        {512 + 1, 1024},
        {1024 + 1, 2048},
    }};

    for (const auto& testCase : cases) {
        void* ptr = allocate(testCase.requestSize);
        ASSERT_NE(ptr, nullptr) << "requestSize=" << testCase.requestSize;

        Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptr));
        ASSERT_NE(span, nullptr) << "requestSize=" << testCase.requestSize;
        EXPECT_EQ(span->objSize, testCase.classSize) << "requestSize=" << testCase.requestSize;
        EXPECT_EQ(getClassSize(span->classSizeIndex), testCase.classSize)
            << "requestSize=" << testCase.requestSize;

        deallocate(ptr);
    }
}

TEST(MemoryPoolTest, PageCacheSplitRollbackOnSetSpanFailure) {
    SetSpanFailureInjectionGuard guard;

    auto held = ReserveFullPageCacheSpans();
    ASSERT_EQ(held.size(), kPageCacheDrainSpanCount);

    Span* fullSpan = held.back();
    held.pop_back();
    ASSERT_NE(fullSpan, nullptr);
    void* expectedPtr = fullSpan->ptr;
    PageCache::getInstance().deallocate(fullSpan);

    failNextNonNullSetSpanAfter(0);
    EXPECT_EQ(PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN - 1), nullptr);

    Span* recovered = PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->ptr, expectedPtr);
    EXPECT_EQ(RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(expectedPtr)), recovered);
    held.push_back(recovered);

    for (Span* heldSpan : held) {
        PageCache::getInstance().deallocate(heldSpan);
    }
}

TEST(MemoryPoolTest, CentralCacheFetchRollbackOnSetSpanFailure) {
    SetSpanFailureInjectionGuard guard;

    Span* pageSpan = PageCache::getInstance().allocate(1);
    ASSERT_NE(pageSpan, nullptr);
    void* expectedPtr = pageSpan->ptr;
    PageCache::getInstance().deallocate(pageSpan);

    size_t count = 1;
    failNextNonNullSetSpanAfter(0);
    EXPECT_EQ(CentralCache::getInstance().allocate(24, getListIndex(24), count), nullptr);

    Span* recovered = PageCache::getInstance().allocate(1);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->ptr, expectedPtr);
    EXPECT_EQ(RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(expectedPtr)), recovered);
    PageCache::getInstance().deallocate(recovered);
}

TEST(MemoryPoolTest, FreeSpanBelowThresholdIsNotReleasedToOS) {
    auto held = ReserveFullPageCacheSpans();
    ASSERT_EQ(held.size(), kPageCacheDrainSpanCount);

    Span* span = held.back();
    held.pop_back();
    ASSERT_NE(span, nullptr);
    void* ptr = span->ptr;

    PageCache::getInstance().deallocate(span);

    Span* freeSpan = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptr));
    ASSERT_NE(freeSpan, nullptr);
    EXPECT_FALSE(freeSpan->isReleasedToOS);
    EXPECT_TRUE(freeSpan->isOsChunkHead);

    Span* recovered = PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->ptr, ptr);
    held.push_back(recovered);

    for (Span* heldSpan : held) {
        PageCache::getInstance().deallocate(heldSpan);
    }
}

TEST(MemoryPoolTest, ScavengerReleasesFreeSpanToOSAndReusesIt) {
    auto held = ReserveFullPageCacheSpans();
    ASSERT_EQ(held.size(), kPageCacheDrainSpanCount);

    std::array<void*, 5> ptrs{};
    for (size_t i = 0; i < ptrs.size(); ++i) {
        ptrs[i] = held.back()->ptr;
        PageCache::getInstance().deallocate(held.back());
        held.pop_back();
    }

    Span* released = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptrs.back()));
    ASSERT_NE(released, nullptr);
    EXPECT_TRUE(released->isReleasedToOS);
    EXPECT_TRUE(released->isOsChunkHead);

    Span* reused = PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->ptr, ptrs.back());
    EXPECT_FALSE(reused->isReleasedToOS);
    held.push_back(reused);

    for (size_t i = 0; i + 1 < ptrs.size(); ++i) {
        Span* span = PageCache::getInstance().allocate(MAX_PAGES_IN_SPAN);
        ASSERT_NE(span, nullptr);
        EXPECT_EQ(span->ptr, ptrs[ptrs.size() - 2 - i]);
        held.push_back(span);
    }

    for (Span* heldSpan : held) {
        PageCache::getInstance().deallocate(heldSpan);
    }
}

TEST(MemoryPoolTest, ScavengerCanReturnWholeChunkToOS) {
    auto held = ReserveFullPageCacheSpans();
    ASSERT_EQ(held.size(), kPageCacheDrainSpanCount);

    std::array<void*, 9> ptrs{};
    for (size_t i = 0; i < ptrs.size(); ++i) {
        ptrs[i] = held.back()->ptr;
        PageCache::getInstance().deallocate(held.back());
        held.pop_back();
    }

    EXPECT_EQ(RadixTreePageMap::getInstance().getSpan(reinterpret_cast<std::uintptr_t>(ptrs.back())), nullptr);

    for (Span* heldSpan : held) {
        PageCache::getInstance().deallocate(heldSpan);
    }
}
