#include "util.h"
#include "CentralCache.h"
#include "PageCache.h"
#include "ThreadCache.h"

#include <cstdlib>
#include <limits>

namespace memorypool {

void ThreadCache::refillFromCentralCache(size_t classSize, size_t index) {
    auto& bucket = buckets_[index];
    size_t count = bucket.threshold;
    auto* applyMemory = CentralCache::getInstance().allocate(classSize, index, count);
    if (applyMemory == nullptr) {
        return;
    }

    bucket.freeList = applyMemory;
    bucket.freeListSize = count;

    if (bucket.threshold <= std::numeric_limits<size_t>::max() / 2) {
        bucket.threshold *= 2;
    }
}

void ThreadCache::flushHalfToCentralCache(size_t index) {
    auto& bucket = buckets_[index];
    const size_t deallocateSize = bucket.freeListSize / 2;
    if (deallocateSize == 0) {
        return;
    }

    std::byte* listHead = bucket.freeList;
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
    bucket.freeList = remaining;
    bucket.freeListSize -= deallocateSize;
    bucket.threshold = bucket.threshold / 2 < DEFAULT_THRESHOLD ? DEFAULT_THRESHOLD : bucket.threshold / 2;
}

void* ThreadCache::allocate(size_t normalizedSize) {
    if (normalizedSize == 0) {
        return nullptr;
    }

    const auto index = getListIndex(normalizedSize);
    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    auto& bucket = buckets_[index];
    if (bucket.freeList == nullptr) {
        const size_t classSize = getClassSize(index);
        if (classSize == 0) {
            return nullptr;
        }
        refillFromCentralCache(classSize, index);
        if (bucket.freeList == nullptr) {
            return nullptr;
        }
    }

    void* ret = static_cast<void*>(bucket.freeList);
    bucket.freeList = *(reinterpret_cast<std::byte**>(ret));
    bucket.freeListSize--;

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

    auto index = span->classSizeIndex;

    if (index >= FREE_LIST_SIZE || span->objSize == 0) {
        return;
    }

    auto& bucket = buckets_[index];
    *(reinterpret_cast<std::byte**>(ptr)) = bucket.freeList;
    bucket.freeList = static_cast<std::byte*>(ptr);
    bucket.freeListSize++;

    if (bucket.freeListSize >= bucket.threshold) {
        flushHalfToCentralCache(index);
    }

    return;
}

ThreadCache::~ThreadCache() {
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i) {
        auto& bucket = buckets_[i];
        if (bucket.freeList != nullptr) {
            CentralCache::getInstance().deallocate(bucket.freeList, i, bucket.freeListSize);
        }
    }
    return;
}

} // memorypool
