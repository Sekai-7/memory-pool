#ifndef _ALLOCATOR_H_
#define _ALLOCATOR_H_

#include "ThreadCache.h"
#include "PageCache.h"

#include <cstddef>

namespace memorypool {
    inline void* allocate(size_t size) {
        const size_t normalizedSize = normalizeSize(size);
        if (normalizedSize > MAX_BYTES) {
            const size_t pageCount = (normalizedSize + PAGE_SIZE - 1) / PAGE_SIZE;
            auto* span = PageCache::getInstance().allocateDirect(pageCount);
            return span == nullptr ? nullptr : span->ptr;
        }
        return ThreadCache::getInstance().allocate(normalizedSize);
    }

    inline void deallocate(void* ptr) {
        if (ptr == nullptr) {
            return;
        }

        Span* span = RadixTreePageMap::getInstance().getSpan(reinterpret_cast<uintptr_t>(ptr));
        if (span == nullptr) {
            return;
        }

        if (span->isDirect) {
            PageCache::getInstance().deallocateDirect(span);
            return;
        }

        ThreadCache::getInstance().deallocate(ptr);
    }
}

#endif
