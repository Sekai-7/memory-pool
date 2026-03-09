#include "CentralCache.h"
#include "PageCache.h"

#include <thread>

namespace memorypool {

std::byte* CentralCache::allocate(size_t size, size_t count) {
    size_t objSize = align(size);
    int index = getListIndex(objSize);
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield(); 
    }

    if (count > centralFreeListSize_[index]) {
        locks_[index].clear(std::memory_order_release);

        size_t bytesNeeded = count * objSize;
        size_t pageCount = (bytesNeeded + PAGE_SIZE - 1) / PAGE_SIZE;

        auto* applySpan = PageCache::getInstance().allocate(pageCount);
        if (applySpan == nullptr) {
            return nullptr;
        }

        std::byte* start = static_cast<std::byte*>(applySpan->ptr);
        size_t totalCount = applySpan->totalSize;

        for (size_t i = 0; i < totalCount - 1; ++i) {
            std::byte* current = start + i * objSize;
            std::byte* nextNode = start + (i + 1) * objSize;
            *reinterpret_cast<std::byte**>(current) = nextNode;
        }

        std::byte* tail = start + (totalCount - 1) * objSize;
        *reinterpret_cast<std::byte**>(tail) = nullptr;

        while (locks_[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield(); 
        }

        *reinterpret_cast<std::byte**>(tail) = centralFreeList_[index];
        centralFreeList_[index] = start;
        centralFreeListSize_[index] += totalCount; 
    } 

    auto* ret = centralFreeList_[index];
    auto next = centralFreeList_[index];
    
    size_t traverseCount = count; 
    while (--traverseCount) {
        next = *(reinterpret_cast<std::byte**>(next));
    }

    centralFreeList_[index] = *(reinterpret_cast<std::byte**>(next));
    *(reinterpret_cast<std::byte**>(next)) = nullptr;
    
    centralFreeListSize_[index] -= count;

    locks_[index].clear(std::memory_order_release);

    return ret;
}

void CentralCache::deallocate(std::byte*, size_t) {

}

CentralCache::~CentralCache() {

}

}