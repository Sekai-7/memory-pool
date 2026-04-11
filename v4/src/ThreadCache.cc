#include "util.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <cstdlib>

namespace memorypool {

void* ThreadCache::allocate(size_t normalizedSize) {
    if (normalizedSize == 0) {
        return nullptr;
    }

    const auto index = getListIndex(normalizedSize);
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    if (freeList_[index] == nullptr) {
        size_t count = getTargetFreeListSizeForIndex(index);
        
        // 从CentralCache申请内存并且分割
        // 中心缓存的分配采取尽力
        auto* applyMemory = CentralCache::getInstance().allocate(normalizedSize, count);
        
        if (applyMemory == nullptr) {
            return nullptr;
        }

        freeList_[index] = applyMemory;
        freeListSize_[index] = count;
    }

    void* ret = static_cast<void*>(freeList_[index]);
    freeList_[index] = *(reinterpret_cast<std::byte**>(ret));
    freeListSize_[index]--;

    return ret;
}

void ThreadCache::deallocate(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<uintptr_t>(ptr));
    if (span == nullptr) {
        return;
    }

    if (span->isDirect) {
        PageCache::getInstance().deallocateDirect(span);
        return;
    }

    size_t size = span->objSize;

    auto index = getListIndex(size);

    if (index >= FREE_LIST_SIZE || size == 0) {
        return;
    }

    *(reinterpret_cast<std::byte**>(ptr)) = freeList_[index];
    freeList_[index] = static_cast<std::byte*>(ptr);
    freeListSize_[index]++;

    const size_t targetFreeListSize = getTargetFreeListSizeForIndex(index);

    if (freeListSize_[index] > targetFreeListSize) {
        size_t keepCount = targetFreeListSize;
        std::byte* listHead = freeList_[index];
        std::byte* listTail = listHead;
        if (keepCount == 0) {
            CentralCache::getInstance().deallocate(listHead, index, freeListSize_[index]);
            freeList_[index] = nullptr;
            freeListSize_[index] = 0;
            return;
        }
        for (size_t i = 1; i < keepCount; ++i) {
            if (listTail == nullptr) {
                return;
            }
            listTail = *(reinterpret_cast<std::byte**>(listTail));
        }
        if (listTail == nullptr) {
            return;
        }

        std::byte* flushHead = *(reinterpret_cast<std::byte**>(listTail));
        *reinterpret_cast<std::byte**>(listTail) = nullptr;
        const size_t flushCount = freeListSize_[index] - keepCount;

        CentralCache::getInstance().deallocate(flushHead, index, flushCount);
        freeListSize_[index] = keepCount;
    }

    return;
}

ThreadCache::~ThreadCache() {
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i) {
        if (freeList_[i] != nullptr) {
            CentralCache::getInstance().deallocate(freeList_[i], i, freeListSize_[i]);
        }
    }
    return;
}

} // memorypool
