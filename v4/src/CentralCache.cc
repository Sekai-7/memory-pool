#include "CentralCache.h"
#include "PageCache.h"

#include <thread>

namespace memorypool {

    std::byte* CentralCache::allocate(size_t size, size_t count) {
        int index = getListIndex(align(size));
        if (index >= FREE_LIST_SIZE) {
            return {};
        }

        while (locks_[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
        }

        std::byte* ret = nullptr;

        if (count > centralFreeListSize_[index]) {
            locks_[index].clear(std::memory_order_release);

            void* applyMemory = PageCache::getInstance().allocate();
            if (applyMemory == nullptr) {
                return nullptr;
            }

        } else {
            ret = centralFreeList_[index];

            auto next = centralFreeList_[index];
            while (--count) {
                next = *(reinterpret_cast<std::byte**>(next));
            }

            centralFreeList_[index] = *(reinterpret_cast<std::byte**>(next));
            *(reinterpret_cast<std::byte**>(next)) = nullptr;
            centralFreeListSize_[index]--;
        }

        locks_[index].clear(std::memory_order_release);

        return ret;
    }

    void CentralCache::deallocate(std::byte*, size_t) {

    }

    CentralCache::~CentralCache() {

    }
}