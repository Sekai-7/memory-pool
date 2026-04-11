#include "PageCache.h"

#include <algorithm>
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
    span->isReleasedToOS = false;
    span->isOsChunkHead = !isDirect;
    span->osChunkPtr = ptr;
    span->osChunkPageCount = pageCount;
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

void PageCache::pushFreeSpan(Span* span) {
    if (span == nullptr) {
        return;
    }
    spanLists_[span->pageCount - 1].pushFront(span);
    freePages_ += span->pageCount;
    if (span->isReleasedToOS) {
        releasedPages_ += span->pageCount;
    }
}

void PageCache::removeFreeSpan(Span* span) {
    if (span == nullptr) {
        return;
    }
    spanLists_[span->pageCount - 1].remove(span);
    freePages_ -= span->pageCount;
    if (span->isReleasedToOS) {
        releasedPages_ -= span->pageCount;
    }
}

void PageCache::refreshOsChunkHead(Span* span) {
    if (span == nullptr || span->isDirect) {
        return;
    }
    span->isOsChunkHead = (span->ptr == span->osChunkPtr) && (span->pageCount == span->osChunkPageCount);
}

bool PageCache::releaseSpanToOS(Span* span) {
    if (span == nullptr || span->isDirect || span->isReleasedToOS || span->pageCount < kMinReleaseSpanPages) {
        return false;
    }

    size_t byteCount = 0;
    if (!checkedPageBytes(span->pageCount, byteCount)) {
        return false;
    }

    if (madvise(span->ptr, byteCount, MADV_DONTNEED) != 0) {
        return false;
    }

    span->isReleasedToOS = true;
    releasedPages_ += span->pageCount;
    return true;
}

bool PageCache::releaseChunkToOS(Span* span) {
    if (span == nullptr || span->isDirect || !span->isOsChunkHead) {
        return false;
    }

    size_t byteCount = 0;
    if (!checkedPageBytes(span->pageCount, byteCount)) {
        return false;
    }

    removeFreeSpan(span);
    clearSpanMap(span->ptr, span->pageCount);
    munmap(span->ptr, byteCount);
    SpanAllocator::getInstance().deallocate(span);
    return true;
}

void PageCache::scavenge() {
    while (freePages_ > releasedPages_ + kScavengeThresholdPages) {
        bool released = false;
        for (size_t idx = MAX_PAGES_IN_SPAN; idx-- > 0;) {
            if ((idx + 1) < kMinReleaseSpanPages) {
                break;
            }
            for (Span* span = spanLists_[idx].front(); span != nullptr; span = spanLists_[idx].next(span)) {
                if (releaseSpanToOS(span)) {
                    released = true;
                    break;
                }
            }
            if (released) {
                break;
            }
        }

        if (!released) {
            break;
        }
    }

    while (freePages_ > kChunkReleaseThresholdPages) {
        bool releasedChunk = false;
        for (size_t idx = MAX_PAGES_IN_SPAN; idx-- > 0;) {
            for (Span* span = spanLists_[idx].front(); span != nullptr;) {
                Span* next = spanLists_[idx].next(span);
                if (releaseChunkToOS(span)) {
                    releasedChunk = true;
                    break;
                }
                span = next;
            }
            if (releasedChunk) {
                break;
            }
        }

        if (!releasedChunk) {
            break;
        }
    }
}

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
            removeFreeSpan(ret);
            const bool wasReleasedToOS = ret->isReleasedToOS;
            if (ret->pageCount > pageCount) {
                Span* splice = SpanAllocator::getInstance().allocate();
                if (splice == nullptr) {
                    pushFreeSpan(ret);
                    return nullptr;
                }

                splice->isDirect = false;
                splice->isFree = true;
                splice->objSize = 0;
                splice->isReleasedToOS = wasReleasedToOS;
                splice->useCount = 0;
                splice->freeList = nullptr;
                splice->prev = nullptr;
                splice->next = nullptr;
                splice->pageCount = ret->pageCount - pageCount;
                splice->ptr = static_cast<std::byte*>(ret->ptr) + pageCount * PAGE_SIZE;
                splice->osChunkPtr = ret->osChunkPtr;
                splice->osChunkPageCount = ret->osChunkPageCount;
                refreshOsChunkHead(splice);

                size_t mappedPages = 0;
                if (!assignSpanMap(splice->ptr, splice->pageCount, splice, mappedPages)) {
                    restoreSpanMap(splice->ptr, mappedPages, ret);
                    SpanAllocator::getInstance().deallocate(splice);
                    pushFreeSpan(ret);
                    return nullptr;
                }

                ret->pageCount = pageCount;
                ret->isReleasedToOS = false;
                refreshOsChunkHead(ret);
                pushFreeSpan(splice);
            }
            ret->isReleasedToOS = false;
            ret->isFree = false;
            ret->isDirect = false;
            refreshOsChunkHead(ret);
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
        pushFreeSpan(ret);
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
        span->isReleasedToOS = false;
        refreshOsChunkHead(span);

        while (true) {
            auto preAddr = reinterpret_cast<uintptr_t>(span->ptr) - PAGE_SIZE;
            auto pre = RadixTreePageMap::getInstance().getSpan(preAddr);
            if (pre == nullptr || pre->isFree == false || pre->isDirect || pre->osChunkPtr != span->osChunkPtr ||
                pre->osChunkPageCount != span->osChunkPageCount || pre->pageCount + span->pageCount > MAX_PAGES_IN_SPAN) {
                break;
            }
            removeFreeSpan(pre);
            pre->pageCount += span->pageCount;
            pre->isReleasedToOS = false;

            for (size_t i = 0; i < span->pageCount; ++i) {
                RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(span->ptr) + i * PAGE_SIZE, pre);
            }

            SpanAllocator::getInstance().deallocate(span);
            span = pre;
            refreshOsChunkHead(span);
        }

        while (true) {
            auto nextAddr = reinterpret_cast<uintptr_t>(span->ptr) + span->pageCount * PAGE_SIZE;
            auto next = RadixTreePageMap::getInstance().getSpan(nextAddr);
            if (next == nullptr || next->isFree == false || next->isDirect || next->osChunkPtr != span->osChunkPtr ||
                next->osChunkPageCount != span->osChunkPageCount || next->pageCount + span->pageCount > MAX_PAGES_IN_SPAN) {
                break;
            }
            removeFreeSpan(next);
            next->ptr = span->ptr;
            next->pageCount += span->pageCount;
            next->isReleasedToOS = false;

            for (size_t i = 0; i < span->pageCount; ++i) {
                RadixTreePageMap::getInstance().setSpan(reinterpret_cast<uintptr_t>(span->ptr) + i * PAGE_SIZE, next);
            }

            SpanAllocator::getInstance().deallocate(span);
            span = next;
            refreshOsChunkHead(span);
        }

        refreshOsChunkHead(span);
        pushFreeSpan(span);
        scavenge();
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
