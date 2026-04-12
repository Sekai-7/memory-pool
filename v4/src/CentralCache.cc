#include "CentralCache.h"
#include "PageCache.h"

#include <algorithm>
#include <thread>

namespace memorypool {

namespace {

constexpr size_t kTargetSpanBytes = 64 * 1024;
constexpr size_t kMaxObjectsPerSpan = 64;

bool remapSpanPages(Span* span) {
    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(span->ptr);
    for (size_t i = 0; i < span->pageCount; ++i) {
        if (!RadixTreePageMap::getInstance().setSpan(startAddr + i * PAGE_SIZE, span)) {
            return false;
        }
    }
    return true;
}

void lockBucket(std::atomic_flag& lock) {
    while (lock.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void unlockBucket(std::atomic_flag& lock) {
    lock.clear(std::memory_order_release);
}

}

std::byte* CentralCache::allocate(size_t size, size_t idx, size_t& count) {
    size_t alignSize = size;

    auto index = idx;

    if (index >= FREE_LIST_SIZE) {
        return nullptr;
    }

    auto& bucket = buckets_[index];
    lockBucket(bucket.lock);

    Span* span = bucket.nonempty.front();
    if (span == nullptr) {
        unlockBucket(bucket.lock);

        span = fetchSpanFromPageCache(alignSize, index);

        if (span == nullptr) {
            return nullptr;
        }

        lockBucket(bucket.lock);
        bucket.nonempty.pushFront(span);
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
        bucket.nonempty.remove(span);
    }

    unlockBucket(bucket.lock);

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

    auto& bucket = buckets_[index];
    lockBucket(bucket.lock);

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
            bucket.nonempty.pushFront(span);
        }

        if (span->useCount == 0) {
            bucket.nonempty.remove(span);
            unlockBucket(bucket.lock);

            // 先不做处理
            PageCache::getInstance().deallocate(span);

            lockBucket(bucket.lock);
        }

        current = next;
    }

    unlockBucket(bucket.lock);

    return;
}

Span* CentralCache::fetchSpanFromPageCache(size_t objSize, size_t index) {
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
    span->classSizeIndex = index; 

    for (size_t i = 1; i < objectCount; ++i) {
        auto* next = current + objSize;
        *reinterpret_cast<std::byte**>(current) = next;
        current = next;
    }
    *reinterpret_cast<std::byte**>(current) = nullptr;

    if (!remapSpanPages(span)) {
        PageCache::getInstance().deallocate(span);
        return nullptr;
    }

    return span;
}

}
