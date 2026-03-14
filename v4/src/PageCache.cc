#include "PageCache.h"

#include <sys/mman.h>

namespace memorypool {

Span* PageCache::allocate(size_t pageCount) {
    {
        std::lock_guard<std::mutex> lock(page_mutex_);
        if (pageCount > MAX_PAGES) {
            return nullptr;
        }
        size_t idx = pageCount - 1;
        while (idx < MAX_PAGES && spanLists_[idx].empty()) {
            ++idx;
        }

        if (idx < MAX_PAGES) {
            auto* ret = spanLists_[idx].front();
            spanLists_[idx].remove(ret);
            if (ret->pageCount > pageCount) {
                Span* splice = SpanAllocator::getInstance().allocate();
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
            return ret;
        }
    }

    size_t size = MAX_PAGES;
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

void PageCache::deallocate(Span* span) {
    if (span == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(page_mutex_);
        spanLists_[span->pageCount - 1].pushFront(span);
    }
    return;
}

Span* PageCache::requestFromOS(size_t pageCount) {
    void* ptr = mmap(nullptr, pageCount * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr != MAP_FAILED) {
        Span* ret = SpanAllocator::getInstance().allocate();
        ret->ptr = ptr;
        ret->pageCount = pageCount;

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