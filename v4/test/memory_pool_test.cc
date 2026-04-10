#include <gtest/gtest.h>

#include "Allocator.h"
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
