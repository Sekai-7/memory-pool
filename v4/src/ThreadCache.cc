#include "util.h"
#include "CentralCache.h"
#include "ThreadCache.h"

#include <cstdlib>

namespace memorypool {

void* ThreadCache::allocate(size_t size) {
    auto alignSize = align(size);
    auto index = getListIndex(alignSize);

    if (index >= FREE_LIST_SIZE) {
        return malloc(size);
    }

    if (freeList_[index] == nullptr) {
        int count = threshold_[index];
        
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

void ThreadCache::deallocate(void* ptr, size_t size) {
    auto index = getListIndex(align(size));

    if (index >= FREE_LIST_SIZE) {
        free(ptr);
        return;
    }

    *(reinterpret_cast<std::byte**>(ptr)) = freeList_[index];
    freeList_[index] = static_cast<std::byte*>(ptr);
    freeListSize_[index]++;

    if (freeListSize_[index] >= threshold_[index]) {
        size_t deallocateSize = freeListSize_[index] / 2;
        auto* next = freeList_[index];
        while (deallocateSize--) {
            if (next == nullptr) {
                // 要做额外处理
                return;
            }
            next = *(reinterpret_cast<std::byte**>(next));
        }
        CentralCache::getInstance().deallocate(freeList_[index], deallocateSize);
        freeList_[index] = next;
        freeListSize_[index] -= deallocateSize;
        threshold_[index] /= 2;
    }

    return;
}

ThreadCache::~ThreadCache() {
    for (int i = 0; i < FREE_LIST_SIZE; ++i) {
        if (freeList_[i] != nullptr) {
            CentralCache::getInstance().deallocate(freeList_[i], freeListSize_[i]);
        }
    }
    return;
}

} // memorypool