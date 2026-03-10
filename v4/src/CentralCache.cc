#include "CentralCache.h"
#include "PageCache.h"

#include <thread>

namespace memorypool {

std::byte* CentralCache::allocate(size_t size, size_t count) {
    size_t alignSize = align(size);
    auto index = getListIndex(alignSize);

    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    Span* span = nonempty_[index].front();
    if (span == nullptr) {
        locks_[index].clear(std::memory_order_release);

        span = fetchSpanFromPageCache(index, alignSize);

        if (span == nullptr) {
            return nullptr;
        }

        while (locks_[index].test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        nonempty_[index].pushFront(span);
    }

    std::byte* head = span->freeList;
    std::byte* tail = head;
    size_t actualCount = 1;

    // 按尽力语义截取链表
    while (actualCount < count && *reinterpret_cast<std::byte**>(tail) != nullptr) {
        tail = *reinterpret_cast<std::byte**>(tail);
        actualCount++;
    }

    // 更新 Span 元数据
    span->freeList = *reinterpret_cast<std::byte**>(tail);
    *reinterpret_cast<std::byte**>(tail) = nullptr; 
    span->useCount += actualCount;                  

    if (span->freeList == nullptr) {
        nonempty_[index].remove(span);
    }

    locks_[index].clear(std::memory_order_release);

    count = actualCount;
}

void CentralCache::deallocate(std::byte*, size_t, size_t) {

}

CentralCache::~CentralCache() {

}

}