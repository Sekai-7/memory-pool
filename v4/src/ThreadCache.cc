#include "util.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <cstdlib>

namespace memorypool {

void* ThreadCache::allocate(size_t size) {
    auto alignSize = normalizeSize(size);
    auto index = getListIndex(alignSize);

    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    if (freeList_[index] == nullptr) {
        size_t count = threshold_[index];
        
        // 从CentralCache申请内存并且分割
        // 中心缓存的分配采取尽力
        auto* applyMemory = CentralCache::getInstance().allocate(alignSize, count);
        
        if (applyMemory == nullptr) {
            return nullptr;
        }

        freeList_[index] = applyMemory;
        freeListSize_[index] = count;

        threshold_[index] *= 2;
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

    auto index = getListIndex(align(size));

    if (index >= FREE_LIST_SIZE || size == 0) {
        return;
    }

    *(reinterpret_cast<std::byte**>(ptr)) = freeList_[index];
    freeList_[index] = static_cast<std::byte*>(ptr);
    freeListSize_[index]++;

    if (freeListSize_[index] >= threshold_[index]) {
        size_t deallocateSize = freeListSize_[index] / 2;
        std::byte* listHead = freeList_[index];
        std::byte* listTail = listHead;
        for (size_t i = 1; i < deallocateSize; ++i) {
            if (listTail == nullptr) {
                return;
            }
            listTail = *(reinterpret_cast<std::byte**>(listTail));
        }
        if (listTail == nullptr) {
            return;
        }

        std::byte* remaining = *(reinterpret_cast<std::byte**>(listTail));
        *reinterpret_cast<std::byte**>(listTail) = nullptr;

        CentralCache::getInstance().deallocate(listHead, index, deallocateSize);
        freeList_[index] = remaining;
        freeListSize_[index] -= deallocateSize;
        threshold_[index] /= 2;
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
