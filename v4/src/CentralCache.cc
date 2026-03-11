#include "CentralCache.h"
#include "PageCache.h"

#include <thread>

namespace memorypool {

std::byte* CentralCache::allocate(size_t size, size_t& count) {
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

    return head;
}

void CentralCache::deallocate(std::byte* listHead, size_t size, size_t count) {
    if (listHead == nullptr) {
        return;
    }

    size_t alignSize = align(size);
    auto index = getListIndex(alignSize);

    if (index >= FREE_LIST_SIZE) {
        return;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::byte* current = listHead;

    while (current != nullptr) {
        std::byte* next = *reinterpret_cast<std::byte**>(current);

        Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<uintptr_t>(current));
        if (span == nullptr) {
            current = next;
            continue;
        }

        bool wasEmpty = (span->freeList == nullptr);
        *reinterpret_cast<std::byte**>(current) = span->freeList;
        span->freeList = current;
        span->useCount--;

        if (wasEmpty) {
            nonempty_[index].pushFront(span);
        }

        if (span->useCount == 0) {
            nonempty_[index].remove(span);
            // 先不做处理
            // PageCache::getInstance().deallocate(span);
        }

        current = next;
    }

    locks_[index].clear(std::memory_order_release);

    return;
}

Span* CentralCache::fetchSpanFromPageCache(int index, size_t objSize) {
}

CentralCache::~CentralCache() {

}


}