#include "PageCache.h"

#include <sys/mman.h>
#include <limits>

namespace memorypool {

namespace {

bool checkedPageBytes(size_t pageCount, size_t& byteCount) {
    if (pageCount == 0 || pageCount > std::numeric_limits<size_t>::max() / PAGE_SIZE) {
        return false;
    }
    byteCount = pageCount * PAGE_SIZE;
    return true;
}

void clearSpanMap(void* ptr, size_t pageCount) {
    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(ptr);
    for (size_t i = 0; i < pageCount; ++i) {
        RadixTreePageMap::getInstance().setSpan(startAddr + i * PAGE_SIZE, nullptr);
    }
}

bool registerSpanMap(Span* span) {
    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(span->ptr);
    for (size_t i = 0; i < span->pageCount; ++i) {
        const uintptr_t pageAddr = startAddr + i * PAGE_SIZE;
        if (!RadixTreePageMap::getInstance().setSpan(pageAddr, span)) {
            clearSpanMap(span->ptr, i);
            return false;
        }
    }
    return true;
}

bool assignSpanMap(void* ptr, size_t pageCount, Span* span, size_t& mappedPages) {
    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(ptr);
    for (; mappedPages < pageCount; ++mappedPages) {
        const uintptr_t pageAddr = startAddr + mappedPages * PAGE_SIZE;
        if (!RadixTreePageMap::getInstance().setSpan(pageAddr, span)) {
            return false;
        }
    }
    return true;
}

void restoreSpanMap(void* ptr, size_t pageCount, Span* span) {
    const uintptr_t startAddr = reinterpret_cast<uintptr_t>(ptr);
    for (size_t i = 0; i < pageCount; ++i) {
        RadixTreePageMap::getInstance().setSpan(startAddr + i * PAGE_SIZE, span);
    }
}

Span* createSpanFromOS(size_t pageCount, bool isFree, bool isDirect) {
    size_t byteCount = 0;
    if (!checkedPageBytes(pageCount, byteCount)) {
        return nullptr;
    }

    void* ptr = mmap(nullptr, byteCount, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }

    Span* span = SpanAllocator::getInstance().allocate();
    if (span == nullptr) {
        munmap(ptr, byteCount);
        return nullptr;
    }

    span->ptr = ptr;
    span->pageCount = pageCount;
    span->objSize = 0;
    span->isFree = isFree;
    span->isDirect = isDirect;
    span->useCount = 0;
    span->freeList = nullptr;

    if (!registerSpanMap(span)) {
        munmap(ptr, byteCount);
        SpanAllocator::getInstance().deallocate(span);
        return nullptr;
    }

    return span;
}

} // namespace

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
                splice->prev = nullptr;
                splice->next = nullptr;
                splice->pageCount = ret->pageCount - pageCount;
                splice->ptr = static_cast<std::byte*>(ret->ptr) + pageCount * PAGE_SIZE;

                size_t mappedPages = 0;
                if (!assignSpanMap(splice->ptr, splice->pageCount, splice, mappedPages)) {
                    restoreSpanMap(splice->ptr, mappedPages, ret);
                    SpanAllocator::getInstance().deallocate(splice);
                    spanLists_[idx].pushFront(ret);
                    return nullptr;
                }

                ret->pageCount = pageCount;
                spanLists_[splice->pageCount - 1].pushFront(splice);
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
    return createSpanFromOS(pageCount, false, true);
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

    size_t byteCount = 0;
    if (checkedPageBytes(span->pageCount, byteCount)) {
        munmap(span->ptr, byteCount);
    }
    SpanAllocator::getInstance().deallocate(span);
}

Span* PageCache::requestFromOS(size_t pageCount) {
    return createSpanFromOS(pageCount, true, false);
}

// PageCache::~PageCache() {
    // return;
// }

}
