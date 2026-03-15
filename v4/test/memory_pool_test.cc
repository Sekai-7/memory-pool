#include <gtest/gtest.h>
#include "Allocator.h"
#include <vector>
#include <thread>

using namespace memorypool;

// 测试基本分配与释放
TEST(MemoryPoolTest, BasicAllocation) {
    void* ptr = allocate(16);
    EXPECT_NE(ptr, nullptr);
    deallocate(ptr, 16);
}

// 测试多次分配与释放
TEST(MemoryPoolTest, MultipleAllocations) {
    std::vector<void*> ptrs;
    const size_t allocSize = 32;
    const int count = 1000;

    for (int i = 0; i < count; ++i) {
        void* ptr = allocate(allocSize);
        EXPECT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    for (void* ptr : ptrs) {
        deallocate(ptr, allocSize);
    }
}

// 测试不同大小的分配
TEST(MemoryPoolTest, DifferentSizesAllocation) {
    std::vector<size_t> sizes = {8, 16, 32, 64, 128, 256, 512, 1024};
    std::vector<void*> ptrs;

    for (size_t size : sizes) {
        void* ptr = allocate(size);
        EXPECT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    for (size_t i = 0; i < sizes.size(); ++i) {
        deallocate(ptrs[i], sizes[i]);
    }
}

// 测试多线程环境下的分配与释放
TEST(MemoryPoolTest, ThreadSafeAllocation) {
    auto worker = []() {
        std::vector<void*> ptrs;
        const size_t allocSize = 64;
        const int count = 1000;

        for (int i = 0; i < count; ++i) {
            void* ptr = allocate(allocSize);
            EXPECT_NE(ptr, nullptr);
            ptrs.push_back(ptr);
        }

        for (void* ptr : ptrs) {
            deallocate(ptr, allocSize);
        }
    };

    const int threadCount = 4;
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(worker);
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}
