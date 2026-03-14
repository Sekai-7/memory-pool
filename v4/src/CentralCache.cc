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

        span = fetchSpanFromPageCache(alignSize);

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

void CentralCache::deallocate(std::byte* listHead, size_t idx, size_t count) {
    if (listHead == nullptr) {
        return;
    }

    auto index = idx;

    if (index >= FREE_LIST_SIZE) {
        return;
    }

    while (locks_[index].test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::byte* current = listHead;

    while (count--) {
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

Span* CentralCache::fetchSpanFromPageCache(size_t objSize) {
    // 由于现在设置的内存比较小，直接默认分配一页内存
    size_t pageCount = 1;

    Span* span = PageCache::getInstance().allocate(pageCount);
    if (span == nullptr) {
        return nullptr;
    }

    pageCount = span->pageCount;

    std::byte* current = static_cast<std::byte*>(span->ptr);
    span->freeList = current;
    span->useCount = 0;
    span->objSize = objSize;

    size_t sum = pageCount * PAGE_SIZE;

    for (size_t i = objSize; i < sum; i += objSize) {
        *reinterpret_cast<std::byte**>(current) = current + objSize;
        current += objSize; 
    }
    *reinterpret_cast<std::byte**>(current) = nullptr;

    for (size_t i = 0; i < pageCount; ++i) {
        RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(static_cast<std::byte*>(span->ptr) + i * PAGE_SIZE), span);
    }


    return span;
}

}