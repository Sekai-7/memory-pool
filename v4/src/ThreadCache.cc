#include "util.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <cstdlib>
#include <limits>

namespace memorypool {

void ThreadCache::refillFromCentralCache(size_t normalizedSize, size_t index) {
    if (threshold_[index] == 0) {
        threshold_[index] = DEFAULT_THRESHOLD;
    }

    size_t count = threshold_[index];
    auto* applyMemory = CentralCache::getInstance().allocate(normalizedSize, index, count);
    if (applyMemory == nullptr) {
        return;
    }

    freeList_[index] = applyMemory;
    freeListSize_[index] = count;

    if (threshold_[index] <= std::numeric_limits<size_t>::max() / 2) {
        threshold_[index] *= 2;
    }
}

void ThreadCache::flushHalfToCentralCache(size_t index) {
    const size_t deallocateSize = freeListSize_[index] / 2;
    if (deallocateSize == 0) {
        return;
    }

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
    threshold_[index] = threshold_[index] / 2 < DEFAULT_THRESHOLD ? DEFAULT_THRESHOLD : threshold_[index] / 2;
}

void* ThreadCache::allocate(size_t normalizedSize) {
    if (normalizedSize == 0) {
        return nullptr;
    }

    const auto index = getListIndex(normalizedSize);
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    if (freeList_[index] == nullptr) {
        refillFromCentralCache(normalizedSize, index);
        if (freeList_[index] == nullptr) {
            return nullptr;
        }
    }

    void* ret = static_cast<void*>(freeList_[index]);
    freeList_[index] = *(reinterpret_cast<std::byte**>(ret));
    freeListSize_[index]--;

    return ret;
}

void ThreadCache::deallocate(void* ptr, Span* span) {
    if (ptr == nullptr || span == nullptr) {
        return;
    }

    if (span->isDirect) {
        PageCache::getInstance().deallocateDirect(span);
        return;
    }

    size_t size = span->objSize;

    auto index = span->classSizeIndex;

    if (index >= FREE_LIST_SIZE || size == 0) {
        return;
    }

    *(reinterpret_cast<std::byte**>(ptr)) = freeList_[index];
    freeList_[index] = static_cast<std::byte*>(ptr);
    freeListSize_[index]++;

    if (threshold_[index] == 0) {
        threshold_[index] = DEFAULT_THRESHOLD;
    }

    if (freeListSize_[index] >= threshold_[index]) {
        flushHalfToCentralCache(index);
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
