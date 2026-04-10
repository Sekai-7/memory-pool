#include "PageCache.h"

#include <sys/mman.h>

namespace memorypool {

Span* PageCache::allocate(size_t pageCount) {
    if (pageCount == 0) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(page_mutex_);
        if (pageCount > MAX_PAGES_IN_SPAN) {
            return nullptr;
        }
        size_t idx = pageCount - 1;
        while (idx < MAX_PAGES_IN_SPAN && spanLists_[idx].empty()) {
            ++idx;
        }

        if (idx < MAX_PAGES_IN_SPAN) {
            auto* ret = spanLists_[idx].front();
            spanLists_[idx].remove(ret);
            if (ret->pageCount > pageCount) {
                Span* splice = SpanAllocator::getInstance().allocate();
                if (splice == nullptr) {
                    spanLists_[idx].pushFront(ret);
                    return nullptr;
                }
                splice->isDirect = false;
                splice->isFree = true;
                splice->objSize = 0;
                splice->useCount = 0;
                splice->freeList = nullptr;
                splice->pageCount = ret->pageCount - pageCount;
                splice->ptr = static_cast<std::byte*>(ret->ptr) + pageCount * PAGE_SIZE;
                spanLists_[splice->pageCount - 1].pushFront(splice);
                ret->pageCount = pageCount;
                uintptr_t spliceAddr = reinterpret_cast<uintptr_t>(splice->ptr);
                for (size_t i = 0; i < splice->pageCount; ++i) {
                    auto pageAddr = static_cast<uintptr_t>(spliceAddr) + i * PAGE_SIZE;
                    RadixTreePageMap::getInstance().setSpan(pageAddr, splice);
                }
            }
            ret->isFree = false;
            ret->isDirect = false;
            return ret;
        }
    }

    size_t size = MAX_PAGES_IN_SPAN;
    Span* ret = requestFromOS(size);
    if (ret == nullptr) {
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(page_mutex_);
        spanLists_[size - 1].pushFront(ret);
    }

    return allocate(pageCount);
}

Span* PageCache::allocateDirect(size_t pageCount) {
    if (pageCount == 0) {
        return nullptr;
    }

    void* ptr = mmap(nullptr, pageCount * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    Span* span = SpanAllocator::getInstance().allocate();
    if (span == nullptr) {
        munmap(ptr, pageCount * PAGE_SIZE);
        return nullptr;
    }

    span->ptr = ptr;
    span->pageCount = pageCount;
    span->objSize = 0;
    span->isFree = false;
    span->isDirect = true;
    span->useCount = 0;
    span->freeList = nullptr;

    uintptr_t startAddr = reinterpret_cast<uintptr_t>(ptr);
    for (size_t i = 0; i < pageCount; ++i) {
        auto pageAddr = static_cast<uintptr_t>(startAddr + i * PAGE_SIZE);
        RadixTreePageMap::getInstance().setSpan(pageAddr, span);
    }

    return span;
}

void PageCache::deallocate(Span* span) {
    if (span == nullptr || span->isDirect) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(page_mutex_);
        span->isFree = true;
        span->objSize = 0;
        span->useCount = 0;
        span->freeList = nullptr;
        span->prev = nullptr;
        span->next = nullptr;

        while (true) {
            auto preAddr = reinterpret_cast<uintptr_t>(span->ptr) - PAGE_SIZE;
            auto pre = RadixTreePageMap::getInstance().getSpan(preAddr);
            if (pre == nullptr || pre->isFree == false || pre->isDirect || pre->pageCount + span->pageCount > MAX_PAGES_IN_SPAN) {
                break;
            }
            spanLists_[pre->pageCount - 1].remove(pre);
            pre->pageCount += span->pageCount;

            for (size_t i = 0; i < span->pageCount; ++i) {
                RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(span->ptr) + i * PAGE_SIZE, pre);
            }

            SpanAllocator::getInstance().deallocate(span);
            span = pre;
        }

        while (true) {
            auto nextAddr = reinterpret_cast<uintptr_t>(span->ptr) + span->pageCount * PAGE_SIZE;
            auto next = RadixTreePageMap::getInstance().getSpan(nextAddr);
            if (next == nullptr || next->isFree == false || next->isDirect || next->pageCount + span->pageCount > MAX_PAGES_IN_SPAN) {
                break;
            }
            spanLists_[next->pageCount - 1].remove(next);
            next->ptr = span->ptr;
            next->pageCount += span->pageCount;

            for (size_t i = 0; i < span->pageCount; ++i) {
                RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(span->ptr) + i * PAGE_SIZE, next);
            }

            SpanAllocator::getInstance().deallocate(span);
            span = next;
        }

        spanLists_[span->pageCount - 1].pushFront(span);
    }
    return;
}

void PageCache::deallocateDirect(Span* span) {
    if (span == nullptr || !span->isDirect) {
        return;
    }

    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(span->ptr);
    for (size_t i = 0; i < span->pageCount; ++i) {
        RadixTreePageMap::getInstance().setSpan(startAddr + i * PAGE_SIZE, nullptr);
    }

    munmap(span->ptr, span->pageCount * PAGE_SIZE);
    SpanAllocator::getInstance().deallocate(span);
}

Span* PageCache::requestFromOS(size_t pageCount) {
    void* ptr = mmap(nullptr, pageCount * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr != MAP_FAILED) {
        Span* ret = SpanAllocator::getInstance().allocate();
        if (ret == nullptr) {
            munmap(ptr, pageCount * PAGE_SIZE);
            return nullptr;
        }
        ret->ptr = ptr;
        ret->pageCount = pageCount;
        ret->objSize = 0;
        ret->isFree = true;
        ret->isDirect = false;
        ret->useCount = 0;
        ret->freeList = nullptr;

        uintptr_t startAddr = reinterpret_cast<uintptr_t>(ptr);
        for (size_t i = 0; i < pageCount; ++i) {
            auto pageAddr = static_cast<uintptr_t>(startAddr + i * PAGE_SIZE);
            RadixTreePageMap::getInstance().setSpan(pageAddr, ret);
        }

        return ret;
    }
    return nullptr;
}

// PageCache::~PageCache() {
    // return;
// }

}
