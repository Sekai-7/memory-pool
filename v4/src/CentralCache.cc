#include "CentralCache.h"
#include "PageCache.h"

#include <algorithm>
#include <thread>

namespace memorypool {

namespace {

constexpr size_t kTargetSpanBytes = 64 * 1024;
constexpr size_t kMaxObjectsPerSpan = 64;

}

std::byte* CentralCache::allocate(size_t size, size_t& count) {
    size_t alignSize = 0;
    if (!normalizeSizeChecked(size, alignSize)) {
        return nullptr;
    }
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
            locks_[index].clear(std::memory_order_release);

            // 先不做处理
            PageCache::getInstance().deallocate(span);

            while (locks_[index].test_and_set(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }

        current = next;
    }

    locks_[index].clear(std::memory_order_release);

    return;
}

Span* CentralCache::fetchSpanFromPageCache(size_t objSize) {
    size_t targetObjects = kTargetSpanBytes / objSize;
    if (targetObjects == 0) {
        targetObjects = 1;
    }
    if (targetObjects > kMaxObjectsPerSpan) {
        targetObjects = kMaxObjectsPerSpan;
    }

    const size_t targetBytes = std::max(objSize * targetObjects, PAGE_SIZE);
    size_t pageCount = (targetBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pageCount == 0) {
        pageCount = 1;
    }
    if (pageCount > MAX_PAGES_IN_SPAN) {
        pageCount = MAX_PAGES_IN_SPAN;
    }

    Span* span = PageCache::getInstance().allocate(pageCount);
    if (span == nullptr) {
        return nullptr;
    }

    const size_t spanBytes = span->pageCount * PAGE_SIZE;
    const size_t objectCount = spanBytes / objSize;
    if (objectCount == 0) {
        PageCache::getInstance().deallocate(span);
        return nullptr;
    }

    std::byte* head = static_cast<std::byte*>(span->ptr);
    std::byte* current = head;
    span->useCount = 0;
    span->objSize = objSize;
    span->isDirect = false;
    span->isFree = false;
    span->freeList = head;

    for (size_t i = 1; i < objectCount; ++i) {
        auto* next = current + objSize;
        *reinterpret_cast<std::byte**>(current) = next;
        current = next;
    }
    *reinterpret_cast<std::byte**>(current) = nullptr;

    for (size_t i = 0; i < span->pageCount; ++i) {
        RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(static_cast<std::byte*>(span->ptr) + i * PAGE_SIZE), span);
    }

    return span;
}

}
